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


class CQPool
{
private:
	std::deque< std::unique_ptr<CompletionQueue> > avail_;
	std::mutex mutex_;

public:
	static CQPool& instance()
	{
		static CQPool inst;
		return inst;
	}

	std::unique_ptr<CompletionQueue> get()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (avail_.empty())
			return std::make_unique<CompletionQueue>();

		auto q = std::move(avail_.front());
		avail_.pop_front();

		return q;
	}

	void release(std::unique_ptr<CompletionQueue>&& cq)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		avail_.push_back(std::move(cq));
	}

private:
	CQPool() = default;
};



class EchoService final : public Echo::Service
{
private:
	std::shared_ptr<Channel> channel_{
		grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials())};
	std::unique_ptr<Echo::Stub> stub_{ Echo::NewStub(channel_) };

	struct EchoCallData
	{
		EchoResponse response;
		ClientContext context;
		Status status;
		std::unique_ptr< ClientAsyncResponseReader<EchoResponse> > response_reader;
		bool done{false};
	};

public:
	EchoService() = default;

	Status Process(ServerContext* context, const EchoRequest* request, EchoResponse* response) override
	{
		static int num_relay = 2;

		using namespace std::chrono;
		auto deadline = system_clock::now() + milliseconds(1000);

		EchoRequest req;
		req.set_message(request->message());

		auto cq = CQPool::instance().get();

		std::vector< std::unique_ptr<EchoCallData> > calls;

		for (size_t i = 0; i < num_relay; i++)
		{
			auto call = std::make_unique<EchoCallData>();

			call->context.set_deadline(deadline);
			auto rpc = stub_->AsyncProcess(&call->context, req, cq.get());
			rpc->Finish(&call->response, &call->status, (void*)i);
			calls.push_back(std::move(call));
		}

		int num = 0;
		while (num != num_relay)
		{
			size_t got_tag;
			bool ok = false;

			GPR_ASSERT(cq->Next((void**)&got_tag, &ok));
			GPR_ASSERT(ok);
			calls[got_tag]->done = true;
			//gpr_log(GPR_INFO, "got event, index:%ld, status:%d", got_tag, calls[got_tag]->status.ok());

			num++;
		}

		auto ndone = std::count_if(
			begin(calls), end(calls), [](auto& i){ return i->done; });
		GPR_ASSERT(ndone == 2);
		auto nok = std::count_if(
			begin(calls), end(calls), [](auto& i){ return i->status.ok(); });
		GPR_ASSERT(nok == 2);

		//usleep(10000);
		response->set_message(request->message());
		CQPool::instance().release(std::move(cq));
		return Status::OK;
	}
};


void runServer()
{
	std::string server_address("0.0.0.0:50050");
	EchoService service;

	ServerBuilder builder;
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);

	std::unique_ptr<Server> server(builder.BuildAndStart());
	gpr_log(GPR_INFO, "Server listening on %s", server_address.c_str());

	server->Wait();
}

int main(int argc, char** argv)
try
{
	gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
	runServer();
	return 0;
}
catch (const std::exception& ex)
{
	gpr_log(GPR_ERROR, "main: %s", ex.what());
	return 1;
}
