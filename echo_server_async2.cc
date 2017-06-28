#include <memory>
#include <iostream>
#include <thread>
#include <string>
#include <functional>
#include <unistd.h>
#include <deque>
#include <signal.h>

#include <grpc++/grpc++.h>
#include <grpc/support/log.h>

#include "echo.grpc.pb.h"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::CompletionQueue;
using grpc::ServerCompletionQueue;
using grpc::Status;

using echo::EchoRequest;
using echo::EchoResponse;
using echo::Echo;


template <typename ServiceType, typename RequestType, typename ResponseType, typename ImplType>
struct ServerRpc
{
protected:
	ServiceType* service_;
	ServerCompletionQueue* cq_;
	ServerContext ctx_;
	RequestType request_;
	ResponseType response_;
	using ResponderType = ServerAsyncResponseWriter<ResponseType>;
	ServerAsyncResponseWriter<ResponseType> responder_;

	std::function<void(bool)> on_read_;
	std::function<void(bool)> on_finish_;

	using InitHandler=std::function<void(ServiceType*,
										 ServerContext*,
										 RequestType*,
										 ResponderType*,
										 CompletionQueue*,
										 ServerCompletionQueue*,
										 void*)>;

public:
	ServerRpc(ServiceType* service, ServerCompletionQueue* cq)
		: service_(service), cq_(cq), responder_(&ctx_)
	{
		on_read_ = [this](bool ok) mutable { this->onRead(ok); };
		on_finish_ = [this](bool ok) mutable { this->onFinish(ok); };
	}

protected:
	void init(const InitHandler& handler)
	{
		handler(service_, &ctx_, &request_, &responder_, cq_, cq_, &on_read_);
	}

private:
	void onRead(bool ok)
	{
		if (!ok)
		{
			std::cout << "---------------" << std::endl;
			delete static_cast<ImplType*>(this);
			return;
		}

		static_cast<ImplType&>(*this).clone();
		static_cast<ImplType&>(*this).process();

		responder_.Finish(response_, Status::OK, &on_finish_);
	}

	void onFinish(bool ok)
	{
		if (!ok)
			std::cout << "===================" << std::endl;
		delete static_cast<ImplType*>(this);
	}
};


struct EchoRpc : public ServerRpc<Echo::AsyncService, EchoRequest, EchoResponse, EchoRpc>
{
private:
	std::string tag_;

public:
	// inheriting constructors
	// using ServerRpc::ServerRpc;

	EchoRpc(Echo::AsyncService* service, ServerCompletionQueue* q, std::string tag)
		: ServerRpc(service, q),
		  tag_(std::move(tag))
	{
		init(&Echo::AsyncService::RequestProcess);
	}

	void process()
	{
		response_.set_message(request_.message() + tag_);
	}

	EchoRpc* clone()
	{
		return new EchoRpc(service_, cq_, tag_);
	}
};


class ThreadPool
{
public:
	struct JobInfo
	{
		std::function<void(bool)>* fn;
		bool ok;
	};

private:
	bool exit_{false};
	std::deque<JobInfo> queue_;
	std::condition_variable cond_;
	mutable std::mutex mutex_;
	std::vector<std::thread> threads_;

public:
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	ThreadPool(size_t num_thread)
	{
		for (auto i = 0; i < num_thread; ++i)
		{
			threads_.emplace_back(&ThreadPool::main, this);
		}
	}

	~ThreadPool() noexcept
	{
		std::cout << "dtor ThreadPool" << std::endl;
		threads_.clear();
		queue_.clear();
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			exit_ = true;
		}
		cond_.notify_all();
		std::for_each(begin(threads_), end(threads_), [](std::thread& t){ t.join(); });

		for (auto job : queue_)
		{
			std::cout << "job finalize" << std::endl;
			(*job.fn)(false);
		}
	}

	void push(std::function<void(bool)>* h, bool ok)
	{
		if (exit_)
		{
			std::cout << "push to finalized worker: ok=" << ok << std::endl;
			(*h)(false);
			return;
		}

		JobInfo info;
		info.fn = h;
		info.ok = ok;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.push_back(std::move(info));
		}
		cond_.notify_one();
	}

private:
	void main()
	{
		std::cout << "worker started" << std::endl;

		while (true)
		{
			JobInfo info;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cond_.wait(lock, [this]{ return exit_ || !queue_.empty(); });

				if (exit_)
					break;

				info = std::move(queue_.front());
				queue_.pop_front();
			}
			(*info.fn)(info.ok);
		}
		std::cout << "worker end" << std::endl;
	}
};


class ServerImpl final
{
private:
	Echo::AsyncService service_;
	std::unique_ptr<ServerCompletionQueue> cq_;
	std::unique_ptr<Server> server_;

	std::vector<std::thread> threads_;
	ThreadPool worker_{2};

public:
	~ServerImpl()
	{
		std::cout << "dtor of ServerImpl" << std::endl;
		worker_.stop();

		server_->Shutdown();
		cq_->Shutdown();

		std::for_each(begin(threads_), end(threads_), [](std::thread& t){ t.join(); });
		threads_.clear();
	}


	void run()
	{
		std::string server_address("0.0.0.0:50051");

		ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service_);
		cq_ = builder.AddCompletionQueue();
		server_ = builder.BuildAndStart();
		std::cout << "Server listening on: " << server_address << std::endl;

		init();

		for (auto i = 0; i < 1; ++i)
		{
			threads_.emplace_back(&ServerImpl::HandleRpcs, this);
		}
	}

	void init()
	{
		new EchoRpc(&service_, cq_.get(), "test");
	}

	void HandleRpcs()
	{
		std::function<void(bool)>* tag;
		bool ok;

		while (true)
		{
			auto ret = cq_->Next((void**)&tag, &ok);
			if (ret == CompletionQueue::NextStatus::GOT_EVENT)
			{
				//if (ok)
				worker_.push(tag, ok);
			}
			else if (ret == CompletionQueue::NextStatus::SHUTDOWN)
			{
				std::cout << "cq shutdown" << std::endl;
				break;
			}
		}

		std::cout << "end of HandleRpcs" << std::endl;

	}
};


int main(int argc, char** argv)
{
	ServerImpl server;
	server.run();

	std::cout << "set signal" << std::endl;
	sigset_t set;
	int signum;
	sigemptyset(&set);
	sigaddset(&set, SIGQUIT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	sigwait(&set, &signum);

	std::cout << "stop server" << std::endl;

	return 0;
}
