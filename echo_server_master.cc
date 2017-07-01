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


class StubPool
{
private:
	std::shared_ptr<Channel> channel_{grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials())};
	std::unique_ptr<Echo::Stub> stub_{ Echo::NewStub(channel_) };

public:
	static StubPool& instance()
	{
		static StubPool inst;
		return inst;
	}

	Echo::Stub* stub()
	{
		return stub_.get();
	}

private:
	StubPool()
	{
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

	~ServerRpc()
	{
		//std::cout << "dtor of ServerRpc" << std::endl;
	}
	void init(const InitHandler& handler)
	{
		handler(service_, &ctx_, &request_, &responder_, cq_, cq_, &on_read_);
	}

private:
	void onRead(bool ok)
	{
		if (!ok)
		{
			delete static_cast<ImplType*>(this);
			return;
		}

		static_cast<ImplType&>(*this).clone();
		static_cast<ImplType&>(*this).process();
	}

	void onFinish(bool ok)
	{
		delete static_cast<ImplType*>(this);
	}
};


struct EchoRpc : public ServerRpc<Echo::AsyncService, EchoRequest, EchoResponse, EchoRpc>
{
private:
	constexpr static int expected_{2}; 	// 하단 서버로 보낼 요청 갯수.

	std::string tag_;
	CompletionQueue* ccq_;
	std::mutex mutex_;

	int num_{0};

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
	}

	~EchoRpc()
	{
		//std::cout << "dtor echo rpc" << std::endl;
	}

	void process()
	{
		using namespace std::chrono;
		auto deadline = system_clock::now() + milliseconds(1000);

		std::lock_guard<std::mutex> lock(mutex_);
		auto stub = StubPool::instance().stub();
		for (auto i = 0; i < expected_; ++i)
		{
			auto call = std::make_unique<ClientCall>();

			call->context.set_compression_algorithm(GRPC_COMPRESS_GZIP);
			call->context.set_deadline(deadline);

			call->fn = [this, i](bool ok){ callback(ok, i); };
			call->response_reader = stub->AsyncProcess(&call->context, request_, ccq_);
			call->response_reader->Finish(&call->response, &call->status, &(call->fn));
			calls_.push_back(std::move(call));
		}
	}

	EchoRpc* clone()
	{
		auto rpc = new EchoRpc(service_, cq_, ccq_, tag_);
		return rpc;
	}

	void callback(bool ok, int i)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!ok)
			{
				gpr_log(GPR_ERROR, "TODO: check this case");
				//delete this;
			}

			//gpr_log(GPR_INFO, "status: %d", calls_[i]->status.ok());

			num_++;
			//gpr_log(GPR_INFO, "callback with index:%d, num:%d", i, num_);
			if (num_ != expected_)
				return;
		}

		// IMPORTANT!! - mutex를 unlock하고 Finish를 호출해야 한다.
		// Finish를 호출하면 on_finish_에서 EchoRpc 객체를 delete 한다.
		// 이 함수(callback)가 종료하기 전에 위 작업이 먼저 수행될 수 있다.
		// 그러면 삭제된 mutex를 unlock 하면서 core가 발생한다.
		response_.set_message(request_.message());
		responder_.Finish(response_, Status::OK, &on_finish_);
	}
};


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

class ServerImpl final
{
private:
	Echo::AsyncService service_;
	std::unique_ptr<Server> server_;

	std::unique_ptr<ServerCompletionQueue> cq_;
	std::unique_ptr<CompletionQueue> ccq_{new CompletionQueue};

	std::vector<std::thread> threads_;

public:
	~ServerImpl()
	{
		gpr_log(GPR_INFO, "shutdown Server");
		server_->Shutdown();

		gpr_log(GPR_INFO, "shutdown ServerCompletionQueue");
		cq_->Shutdown();

		gpr_log(GPR_INFO, "shutdown Client CompletionQueue");
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

		// 클라이언트의 연결을 담당하는 thread (server thread)는 네트웍 I/O만을 담당한다.
		// 이경우 thread를 늘리면 오히려 성능이 저하된다.
		// 성능을 끌어내기 위해서는 ServerCompletionQueue를 thread 당 하나씩 할당해야 한다.
		for (auto i = 0; i < 1; ++i)
		{
			threads_.emplace_back(&ServerImpl::HandleRpcs, this);
		}

		// 하단 서버(slave) 와의 연결을 담당하는 thread는 sort/merge 등을 처리해야 한다.
		// I/O 작업만 수행하는 ServerCompletionQueue와 달리 thread 갯수를 늘릴 수 있다.
		// 단, sort/merge 작업의 부하가 작으면 thread 갯수가 작을 수록 유리하다.
		// 성능을 끌어내기 위해서는 CompletionQueue를 thread 당 하나씩 할당해야 한다.
		for (auto i = 0; i< 4; ++i)
		{
			threads_.emplace_back(&ServerImpl::HandleClients, this);
		}
	}

	void init()
	{
		new EchoRpc(&service_, cq_.get(), ccq_.get(), "test");
	}

	void HandleRpcs()
	{
		// 종료 signal을 worker thread가 받으면 안된다.
		SignalBlocker({SIGQUIT, SIGTERM, SIGINT});
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
				gpr_log(GPR_INFO, "ServerCompletionQueue shutdowned");
				break;
			}
		}

		gpr_log(GPR_INFO, "end of HandleRpcs");
	}

	void HandleClients()
	{
		// 종료 signal을 worker thread가 받으면 안된다.
		SignalBlocker({SIGQUIT, SIGTERM, SIGINT});
		std::function<void(bool)>* tag;
		bool ok;

		while (true)
		{
			auto ret = ccq_->Next((void**)&tag, &ok);
			if (ret == CompletionQueue::NextStatus::GOT_EVENT)
			{
				//gpr_log(GPR_DEBUG, "got client event with ok=%d", ok);
				(*tag)(ok);
				//ThreadPool::instance().push(tag);
			}
			else if (ret == CompletionQueue::NextStatus::SHUTDOWN)
			{
				gpr_log(GPR_INFO, "Client CompletionQueue shutdowned");
				break;
			}
		}
		gpr_log(GPR_INFO, "end of HandleClients");
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
{
	gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);

	ServerImpl server;
	server.run();

	// connect to slave
	StubPool::instance().stub();

	gpr_log(GPR_INFO, "Server started");
	wait({SIGQUIT, SIGTERM, SIGINT});
	gpr_log(GPR_INFO, "stop Server");

	return 0;
}
