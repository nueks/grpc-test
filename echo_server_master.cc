#include <memory>
#include <iostream>
#include <thread>
#include <string>
#include <functional>
#include <unistd.h>
#include <deque>
#include <signal.h>
#include <chrono>

#include <grpc++/grpc++.h>
#include <grpc/support/log.h>

#include "echo.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Server;
using grpc::ClientAsyncResponseReader;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::CompletionQueue;
using grpc::ServerCompletionQueue;
using grpc::Status;

using echo::EchoRequest;
using echo::EchoResponse;
using echo::Echo;

/*
class ThreadPool
{
private:
	struct JobInfo
	{
		std::function<void(bool)>* fn;
	};

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
		std::cout << "dtor ThreadPool" << std::endl;
		threads_.clear();
		queue_.clear();
	}

	void init(size_t num_thread)
	{
		for (auto i = 0; i < num_thread; ++i)
		{
			threads_.emplace_back(&ThreadPool::main, this);
		}
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
			(*job)(false);
		}
	}

	void push(std::function<void(bool)>* h)
	{
		if (exit_)
		{
			std::cout << "push to finalized worker" << std::endl;
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
	void main()
	{
		std::cout << "worker started" << std::endl;
		while (true)
		{
			//JobInfo info;
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
		std::cout << "worker end" << std::endl;
	}
};
*/

template <typename ServiceType, typename RequestType, typename ResponseType, typename ImplType>
struct ServerRpc// : public std::enable_shared_from_this<ServerRpc<ServiceType, RequestType, ResponseType, ImplType> >
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
		//on_read_ = [this](bool ok) mutable { this->onRead(ok); };
		//on_finish_ = [this](bool ok) mutable { this->onFinish(ok); };


		// job_ = [this](bool ok){
		// 	if (!ok)
		// 	{
		// 		delete static_cast<ImplType*>(this);
		// 	}
		// 	else
		// 	{
		// 		static_cast<ImplType&>(*this).process();
		// 		responder_.Finish(response_, Status::OK, &on_finish_);
		// 	}
		// };
	}

	~ServerRpc()
	{
		//std::cout << "dtor of ServerRpc" << std::endl;
	}
	void init(const InitHandler& handler)
	{
		on_read_ = [this](bool ok) mutable { this->onRead(ok); };
		on_finish_ = [this](bool ok) mutable { this->onFinish(ok); };

		//on_read_ = [self = this->shared_from_this()](bool ok) mutable { self->onRead(ok); };
		//on_finish_ = [self = this->shared_from_this()](bool ok) mutable { self->onFinish(ok); };

		handler(service_, &ctx_, &request_, &responder_, cq_, cq_, &on_read_);
	}

private:
	void onRead(bool ok)
	{
		if (!ok)
		{
			//std::cout << "---------------" << std::endl;
			//delete static_cast<ImplType*>(this);
			return;
		}

		static_cast<ImplType&>(*this).clone();
		//ThreadPool::instance().push(&job_);
		static_cast<ImplType&>(*this).process();
	}

	void onFinish(bool ok)
	{
		//std::cout << this->shared_from_this().use_count() << std::endl;
		//if (!ok)
		//	std::cout << "===================" << std::endl;
		delete static_cast<ImplType*>(this);
	}
};



class Pool
{
public:
	static Pool& instance()
	{
		static Pool inst;
		return inst;
	}

private:
	Pool()
	{
		//channel_ = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
		//stub_ = Echo::NewStub(channel_);
	}

public:
	Echo::Stub* stub()
	{
		return stub_.get();
	}

private:
	std::shared_ptr<Channel> channel_{grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials())};
	std::unique_ptr<Echo::Stub> stub_{ Echo::NewStub(channel_) };
};


struct EchoRpc : public ServerRpc<Echo::AsyncService, EchoRequest, EchoResponse, EchoRpc>
{
private:
	std::string tag_;
	CompletionQueue* ccq_;
	std::mutex mutex_;

	int num_{0};

	//ClientContext context_;
	struct ClientCall
	{
		ClientContext context;
		EchoResponse response;
		Status status;
		std::unique_ptr< ClientAsyncResponseReader<EchoResponse> > response_reader;
		std::function<void(bool)> fn;
	};

	std::vector< std::unique_ptr<ClientCall> > calls_;

public:
	EchoRpc(Echo::AsyncService* service, ServerCompletionQueue* q, CompletionQueue* cq, std::string tag)
		: ServerRpc(service, q),
		  tag_(std::move(tag)),
		  ccq_(cq)
	{
		init(&Echo::AsyncService::RequestProcess);
		//context_.set_compression_algorithm(GRPC_COMPRESS_GZIP);
		//fn_ = [this](bool ok){ callback(ok); };
	}

	~EchoRpc()
	{
		//std::cout << "dtor echo rpc" << std::endl;
	}

	void process()
	{
		//gpr_log(GPR_DEBUG, "process");

		using namespace std::chrono;

		system_clock::time_point deadline = system_clock::now() + milliseconds(1000);
		//context_.set_deadline(deadline);

		std::lock_guard<std::mutex> lock(mutex_);
		auto stub = Pool::instance().stub();
		for (auto i = 0; i < 2; ++i)
		{
			//gpr_log(GPR_DEBUG, "send to slave: %d", i);

			auto call = std::make_unique<ClientCall>();

			//ClientCall call;
			call->context.set_compression_algorithm(GRPC_COMPRESS_GZIP);
			call->context.set_deadline(deadline);

			call->fn = [this, i](bool ok){ callback(ok, i); };
			call->response_reader = stub->AsyncProcess(&call->context, request_, ccq_);
			call->response_reader->Finish(&call->response, &call->status, &(call->fn));
			calls_.push_back(std::move(call));
		}
	}

	EchoRpc* clone()
	//std::shared_ptr<EchoRpc> clone()
	{
		//return new EchoRpc(service_, cq_, ccq_, tag_);
		//return std::make_shared<EchoRpc>(service_, cq_, ccq_, tag_);
		//init(&Echo::AsyncService::RequestProcess);
		//auto rpc = std::make_shared<EchoRpc>(service_, cq_, ccq_, tag_);
		auto rpc = new EchoRpc(service_, cq_, ccq_, tag_);
		//rpc->init(&Echo::AsyncService::RequestProcess);
		return rpc;

	}

	void callback(bool ok, int i)
	{
		// 끊기거나, timeout 이 발생해도 이벤트가 발생해야 한다.
		// 등록한 이벤트를 모두 받은 다음에만 삭제 할 수 있다.

		// 한 쓰레드가 callback 처리중인데, 다른 쓰레드가 먼저 callback 을 처리하고 delete 까지 한다면?
		{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!ok)
		{
			// .......

			gpr_log(GPR_ERROR, "TODO: check this case");
			//delete this;

		}

		//gpr_log(GPR_INFO, "status: %d", calls_[i]->status.ok());



		num_++;
		//gpr_log(GPR_INFO, "callback with index:%d, num:%d", i, num_);
		if (num_ != 2)
			return;
		}
		if (num_ == 2)
		{
			//usleep(10000);
			//gpr_log(GPR_DEBUG, "response to client");
			response_.set_message(request_.message());
			responder_.Finish(response_, Status::OK, &on_finish_);
		}
	}
};


class ServerImpl final
{
private:
	Echo::AsyncService service_;
	std::unique_ptr<CompletionQueue> ccq_{new CompletionQueue};
	std::unique_ptr<ServerCompletionQueue> cq_;
	std::unique_ptr<Server> server_;

	std::vector<std::thread> threads_;

public:
	~ServerImpl()
	{
		//std::cout << "dtor of ServerImpl" << std::endl;
		gpr_log(GPR_INFO, "dtor of ServerImpl");

		server_->Shutdown();

		gpr_log(GPR_INFO, "shutdown cq");
		cq_->Shutdown();

		gpr_log(GPR_INFO, "shutdown ccq");
		ccq_->Shutdown();

		std::for_each(begin(threads_), end(threads_), [](std::thread& t){ t.join(); });
		threads_.clear();
	}


	void run()
	{
		std::string server_address("0.0.0.0:50050");

		ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service_);
		cq_ = builder.AddCompletionQueue();
		server_ = builder.BuildAndStart();

		gpr_log(GPR_INFO, "Server listening on: %s", server_address.c_str());

		init();

		for (auto i = 0; i < 1; ++i)
		{
			threads_.emplace_back(&ServerImpl::HandleRpcs, this);
		}

		for (auto i = 0; i< 8; ++i)
		{
			threads_.emplace_back(&ServerImpl::HandleClients, this);
		}
	}

	void init()
	{
		auto rpc = new EchoRpc(&service_, cq_.get(), ccq_.get(), "test");
		//auto rpc = std::make_shared<EchoRpc>(&service_, cq_.get(), ccq_.get(), "test");
		//rpc->init(&Echo::AsyncService::RequestProcess);
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
				//ThreadPool::instance().push(tag);
			}
			else if (ret == CompletionQueue::NextStatus::SHUTDOWN)
			{
				//std::cout << "cq shutdown" << std::endl;
				//gpr_log(GPR_INFO, "cq shutdowned");
				break;
			}
		}

		//std::cout << "end of HandleRpcs" << std::endl;
		gpr_log(GPR_INFO, "end of HandleRpcs");
	}

	void HandleClients()
	{
		std::function<void(bool)>* tag;
		bool ok;

		while (true)
		{
			auto ret = ccq_->Next((void**)&tag, &ok);
			//gpr_log(GPR_DEBUG, "got client event. ret = %d", ret);
			if (ret == CompletionQueue::NextStatus::GOT_EVENT)
			{
				//gpr_log(GPR_DEBUG, "got client event with ok=%d", ok);
				(*tag)(ok);
				//ThreadPool::instance().push(tag);
			}
			else if (ret == CompletionQueue::NextStatus::SHUTDOWN)
			{
				//std::cout << "ccq shutdown" << std::endl;
				//gpr_log(GPR_INFO, "ccq shutdowned");
				break;
			}
		}
		gpr_log(GPR_INFO, "end of HandleClients");
		//std::cout << "end of HandleClients" << std::endl;
	}
};


int main(int argc, char** argv)
{
	gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
	Pool::instance().stub();
	//ThreadPool::instance().init(8);
	ServerImpl server;
	server.run();

	//std::cout << "set signal" << std::endl;
	gpr_log(GPR_INFO, "wait signal");
	sigset_t set;
	int signum;
	sigemptyset(&set);
	sigaddset(&set, SIGQUIT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	sigwait(&set, &signum);

	gpr_log(GPR_INFO, "stop server");
	//std::cout << "stop server" << std::endl;
	//ThreadPool::instance().stop();

	return 0;
}
