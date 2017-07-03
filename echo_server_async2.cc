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


class SignalBlocker
{
private:
	bool blocked_ = false;
	sigset_t mask_;
	sigset_t old_mask_;

public:
	SignalBlocker(SignalBlocker&) = delete;
	SignalBlocker& operator=(SignalBlocker&) = delete;

	SignalBlocker(std::initializer_list<int> sigs)
	{
		sigemptyset(&mask_);
		for (auto sig : sigs)
		{
			sigaddset(&mask_, sig);
		}

		block();
	}

	~SignalBlocker()
	{
		unblock();
	}

	void block()
	{
		if (!blocked_)
		{
			blocked_ = (::pthread_sigmask(SIG_BLOCK, &mask_, &old_mask_) == 0);
		}
	}

	void unblock()
	{
		if (blocked_)
		{
			blocked_ = (::pthread_sigmask(SIG_SETMASK, &old_mask_, 0) != 0);
		}
	}
};


// CompletionQueue를 여러 thread가 polling 하는 것 보다
// 한 thread가 polling 하고, 별도의 thread pool을 사용하는 방식이
// 성능이 더 잘나온다.
class ThreadPool
{
private:
	bool exit_{false};
	std::deque< std::function<void(bool)>* > queue_;
	std::condition_variable cond_;
	mutable std::mutex mutex_;
	std::vector<std::thread> threads_;

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
	ThreadPool() = default;

public:
	static ThreadPool& instance()
	{
		static ThreadPool instance;
		return instance;
	}

	~ThreadPool() noexcept
	{
		gpr_log(GPR_INFO, "destructor of ThreadPool");
		threads_.clear();
		queue_.clear();
	}

	void init(size_t num_thread)
	{
		for (auto i = 0; i < num_thread; ++i)
		{
			threads_.emplace_back([this, i](){ this->main(i); });
		}
	}

	void stop()
	{
		gpr_log(GPR_INFO, "stop ThreadPool");
		{
			std::lock_guard<std::mutex> lock(mutex_);
			exit_ = true;
		}
		cond_.notify_all();
		std::for_each(begin(threads_), end(threads_), [](std::thread& t){ t.join(); });

		for (auto job : queue_)
		{
			gpr_log(GPR_INFO, "finalize unfinished job");
			(*job)(false);
		}
	}

	void push(std::function<void(bool)>* h)
	{
		if (exit_)
		{
			gpr_log(GPR_INFO, "push to finalized ThreadPool");
			(*h)(false);
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.emplace_back(h);
		}
		cond_.notify_one();
	}

private:
	void main(int index)
	{
		// 종료 signal을 worker thread가 받으면 안된다.
		SignalBlocker({SIGQUIT, SIGTERM, SIGINT});

		gpr_log(GPR_INFO, "Worker started (num:%d)", index);
		while (true)
		{
			std::function<void(bool)>* job;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cond_.wait(lock, [this]{ return exit_ || !queue_.empty(); });

				if (exit_)
					break;

				job = std::move(queue_.front());
				queue_.pop_front();
			}
			(*job)(true);
		}
		gpr_log(GPR_INFO, "Worker end (num:%d)", index);
	}
};



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

	std::function<void(bool)> job_;

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
		job_ = [this](bool ok){
			if (!ok)
			{
				delete static_cast<ImplType*>(this);
			}
			else
			{
				static_cast<ImplType&>(*this).process();
				responder_.Finish(response_, Status::OK, &on_finish_);
			}
		};
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
			gpr_log(GPR_DEBUG, "onRead: no ok");
			delete static_cast<ImplType*>(this);
			return;
		}

		static_cast<ImplType&>(*this).clone();

		// 아래 코드가 이번 구현의 핵심이다.
		// 직접 job 을 실행하는 것이아니고 ThreadPool에 위임한다.
		ThreadPool::instance().push(&job_);

		//static_cast<ImplType&>(*this).process();
		//responder_.Finish(response_, Status::OK, &on_finish_);
	}

	void onFinish(bool ok)
	{
		if (!ok)
			gpr_log(GPR_DEBUG, "onFinish: no ok (no error)");

		delete static_cast<ImplType*>(this);
	}
};


struct EchoRpc : public ServerRpc<Echo::AsyncService, EchoRequest, EchoResponse, EchoRpc>
{
private:
	std::string tag_;

public:
	EchoRpc(Echo::AsyncService* service, ServerCompletionQueue* q, std::string tag)
		: ServerRpc(service, q),
		  tag_(std::move(tag))
	{
		init(&Echo::AsyncService::RequestProcess);
	}

	void process()
	{
		//usleep(1000);
		response_.set_message(request_.message() + tag_);
	}

	EchoRpc* clone()
	{
		return new EchoRpc(service_, cq_, tag_);
	}
};



class ServerImpl final
{
private:
	Echo::AsyncService service_;
	std::unique_ptr<ServerCompletionQueue> cq_;
	std::unique_ptr<Server> server_;

	std::vector<std::thread> threads_;

public:
	~ServerImpl()
	{
		gpr_log(GPR_INFO, "destructor of ServerImpl");

		gpr_log(GPR_INFO, "shutdown Server");
		server_->Shutdown();

		gpr_log(GPR_INFO, "shutdown ServerCompletionQueue");
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
		gpr_log(GPR_INFO, "Server listening on: %s", server_address.c_str());

		init();

		// IO thread는 하나만!
		threads_.emplace_back(&ServerImpl::HandleRpcs, this);
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
				(*tag)(ok);
			}
			else if (ret == CompletionQueue::NextStatus::SHUTDOWN)
			{
				gpr_log(GPR_DEBUG, "ServerCompletionQueue shutdowned");
				break;
			}
		}

		gpr_log(GPR_INFO, "end of HandleRpcs");
	}
};


void wait(std::initializer_list<int> sigs)
{
	sigset_t set;
	int signum;
	sigemptyset(&set);
	for (auto sig : sigs)
	{
		sigaddset(&set, sig);
	}
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	sigwait(&set, &signum);
}


int main(int argc, char** argv)
try
{
	gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);

	ThreadPool::instance().init(16);

	ServerImpl server;
	server.run();

	gpr_log(GPR_INFO, "Server started");
	wait({SIGQUIT, SIGTERM, SIGINT});
	gpr_log(GPR_INFO, "stop Server");

	ThreadPool::instance().stop();
	return 0;
}
catch (const std::exception& ex)
{
	gpr_log(GPR_ERROR, "main: %s", ex.what());
	return 1;
}
