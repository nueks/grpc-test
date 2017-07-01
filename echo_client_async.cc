#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <unistd.h>

#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <grpc++/grpc++.h>
#include <grpc/support/log.h>

#include "echo.grpc.pb.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::Status;
using grpc::CompletionQueue;

using echo::EchoRequest;
using echo::EchoResponse;
using echo::Echo;

class EchoClient
{
private:

	std::unique_ptr<Echo::Stub> stub_;
	CompletionQueue cq_;

	struct AsyncClientCall
	{
		EchoResponse response;
		ClientContext context;
		Status status;
		std::unique_ptr< ClientAsyncResponseReader<EchoResponse> > response_reader;
	};

public:
	explicit EchoClient(std::shared_ptr<Channel> channel)
		: stub_(Echo::NewStub(channel))
	{
	}

	std::string echo(const std::string& message, CompletionQueue& cq)
	{
		AsyncClientCall* call = new AsyncClientCall;
		EchoRequest request;
		request.set_message(message);

		EchoResponse response;
		ClientContext context;
		//CompletionQueue cq;
		Status status;

		using namespace std::chrono;
		system_clock::time_point deadline = system_clock::now() + milliseconds(3000);
		context.set_deadline(deadline);

		std::unique_ptr<ClientAsyncResponseReader<EchoResponse> > rpc(
			stub_->AsyncProcess(&context, request, &cq)
		);
		rpc->Finish(&response, &status, (void*)call);

		void* got_tag;
		bool ok = false;

		GPR_ASSERT(cq.Next(&got_tag, &ok));
		GPR_ASSERT(got_tag == (void*)call);
		GPR_ASSERT(ok);

		delete call;

		if (status.ok())
			return response.message();
		else
			return "RPC failed";
	}


	void echo_async(const std::string& message)
	{
		EchoRequest request;
		request.set_message(message);

		AsyncClientCall* call = new AsyncClientCall;
		using namespace std::chrono;
		system_clock::time_point deadline = system_clock::now() + milliseconds(3000);
		call->context.set_deadline(deadline);
		call->response_reader = stub_->AsyncProcess(&call->context, request, &cq_);
		call->response_reader->Finish(&call->response, &call->status, (void*)call);
	}

	void AsyncCompleteRpc()
	{
		void* got_tag;
		bool ok = false;

		while (cq_.Next(&got_tag, &ok))
		{

			AsyncClientCall* call = static_cast<AsyncClientCall*>(got_tag);
			GPR_ASSERT(ok);
			if (!call->status.ok())
				std::cout << "RPC failed" << std::endl;

			delete call;
		}
	}


};

class Bnch
{
private:
	std::atomic_size_t result_{0};
	std::atomic_bool exit_{false};
	std::vector<std::thread> threads_;
	//std::unique_ptr<std::thread> io_;

	std::string message_{"test message blablabla"};

	EchoClient client_;

public:
	Bnch(const Bnch&) = delete;
	Bnch& operator=(const Bnch&) = delete;

	Bnch()
		: client_(grpc::CreateChannel("localhost:50050", grpc::InsecureChannelCredentials()))
		  //io_(&EchoClient::AsyncCompleteRpc, &client_)
	{
		std::cout << "1" << std::endl;
		for (auto i = 0; i < 16; ++i)
		{
			threads_.emplace_back(&Bnch::main, this);
		}
		std::cout << "2" << std::endl;

		//io_.reset(new std::thread(&EchoClient::AsyncCompleteRpc, &client_));
	}

	~Bnch()
	{
		stop();
	}

	void stop()
	{
		exit_ = true;
		std::for_each(begin(threads_), end(threads_), [](std::thread& t){ t.join(); });
		threads_.clear();
		//io_->join();
	}

	size_t result() const
	{
		return result_.load();
	}

	bool ok() const
	{
		return !exit_;
	}

	void main()
	try
	{
		CompletionQueue cq;
		while (!exit_)
		{
			//std::cout << "10" << std::endl;
			client_.echo(message_, cq);
			result_++;
		}
	}
	catch (const std::exception& ex)
	{
		std::cout << "thread main: " << ex.what() << std::endl;
		exit_ = true;
	}


};

int main(int argc, char** argv)
try
{
	std::cout << "11" << std::endl;
	Bnch bnch;
	std::cout << "22" << std::endl;

	auto periodic = [](long sec) {
		int timer = timerfd_create(CLOCK_MONOTONIC, 0);
		if (timer == -1)
			throw std::runtime_error("timerfd create failed");

		struct itimerspec itval;
		itval.it_interval.tv_sec = sec;
		itval.it_interval.tv_nsec = 0;
		itval.it_value.tv_sec = sec;
		itval.it_value.tv_nsec = 0;

		auto ret = timerfd_settime(timer, 0, &itval, NULL);
		if (ret == -1)
			throw std::runtime_error("settime failed");

		return timer;
	};

	// EchoClient client(
	// 	grpc::CreateChannel(
	// 		"localhost:50051", grpc::InsecureChannelCredentials()
	// 	)
	// );

	// std::thread thread_ = std::thread(&EchoClient::AsyncCompleteRpc, &client);

	// for (int i = 0; i < 100; i++) {
	// 	client.echo_async("test");
	// }

	// thread_.join();




	auto timer = periodic(1);
	size_t before = 0;

	while (true)
	{
		if (!bnch.ok())
			throw std::runtime_error("bnch failed");

		uint64_t missed{0};
		auto ret = read(timer, &missed, sizeof(missed));
		if (ret == -1)
			throw std::runtime_error("read timer");

		auto now = bnch.result();
		std::cout << "processed: " << now - before << std::endl;
		before = now;
	}

	return 0;
}
catch (const std::exception& ex)
{
	std::cout << "main: " << ex.what() << std::endl;
	return 1;
}
