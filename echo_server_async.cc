#include <memory>
#include <iostream>
#include <thread>
#include <string>
#include <functional>
#include <unistd.h>
#include <deque>
#include <signal.h>
#include <atomic>

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
			delete static_cast<ImplType*>(this);
			return;
		}

		static_cast<ImplType&>(*this).clone();
		static_cast<ImplType&>(*this).process();

		responder_.Finish(response_, Status::OK, &on_finish_);
	}

	void onFinish(bool /* ok */)
	{
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
	std::vector <std::unique_ptr<ServerCompletionQueue> > cqs_;

	std::vector<std::thread> threads_;

public:
	~ServerImpl()
	{
		gpr_log(GPR_INFO, "destructor of ServerImpl");

		server_->Shutdown();
		std::for_each(
			begin(cqs_), end(cqs_),
			[](std::unique_ptr<ServerCompletionQueue>& cq){ cq->Shutdown(); }
		);
		std::for_each(
			begin(threads_), end(threads_),
			[](std::thread& t){ t.join(); }
		);
		threads_.clear();

		gpr_log(GPR_INFO, "ServerImpl destructed");
	}


	void run()
	{
		int num_cq = 4;
		std::string server_address("0.0.0.0:50051");

		ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service_);

		for (auto i = 0; i < num_cq; ++i)
		{
			cqs_.push_back(builder.AddCompletionQueue());
		}
		server_ = builder.BuildAndStart();
		gpr_log(GPR_INFO, "Server listening on: %s", server_address.c_str());

		init(num_cq);

		for (auto i = 0; i < num_cq; ++i)
		{
			threads_.emplace_back([this, i](){ this->HandleRpcs(i); });
			threads_.emplace_back([this, i](){ this->HandleRpcs(i); });
		}
	}

	void init(int index)
	{
		for (auto i = 0; i < index; ++i)
		{
			new EchoRpc(&service_, cqs_[i].get(), "test");
		}
	}

	void HandleRpcs(int index)
	{
		// 종료 signal을 worker thread가 받으면 안된다.
		SignalBlocker({SIGQUIT, SIGTERM, SIGINT});

		std::function<void(bool)>* tag;
		bool ok;

		gpr_log(GPR_INFO, "HandleRpcs (thread:%d)", index);
		while (true)
		{
			auto ret = cqs_[index]->Next((void**)&tag, &ok);
			if (ret == CompletionQueue::NextStatus::GOT_EVENT)
			{
				(*tag)(ok);
			}
			else if (ret == CompletionQueue::NextStatus::SHUTDOWN)
			{
				//gpr_log(GPR_INFO, "CompletionQueue shutdowned for thread:%d", index);
				break;
			}
			else
			{
				gpr_log(GPR_ERROR, "Unknown event: %d", ret);
			}
		}

		gpr_log(GPR_INFO, "End of HandleRpcs (thread:%d)", index);
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

	ServerImpl server;
	server.run();

	gpr_log(GPR_INFO, "Server started");
	wait({SIGQUIT, SIGTERM, SIGINT});
	gpr_log(GPR_INFO, "stop Server");
	return 0;
}
catch (const std::exception& ex)
{
	gpr_log(GPR_ERROR, "main: %s", ex.what());
	return 1;
}
