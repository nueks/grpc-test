#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>

#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <grpc++/grpc++.h>
#include <grpc/support/log.h>
#include "echo.grpc.pb.h"

#include <gflags/gflags.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using echo::EchoRequest;
using echo::EchoResponse;
using echo::Echo;

DEFINE_string(address, "localhost:50050", "target address to request");
DEFINE_int32(workers, 32, "number of workers");

class EchoClient
{
private:
	std::unique_ptr<Echo::Stub> stub_;

public:
	EchoClient(std::shared_ptr<Channel> channel)
		: stub_(Echo::NewStub(channel))
	{}

	std::string echo(const std::string& message)
	{
		EchoRequest request;
		request.set_message(message);

		EchoResponse response;

		using namespace std::chrono;
		system_clock::time_point deadline = system_clock::now() + milliseconds(1000);
		ClientContext context;
		context.set_deadline(deadline);
		Status status  = stub_->Process(&context, request, &response);

		if (status.ok())
			return response.message();
		else
		{
			std::cout << status.error_code() << ": " << status.error_message() << std::endl;
			return "RPC failed";
		}
	}
};

class Bnch
{
private:
	std::atomic_size_t result_{0};
	std::atomic_bool exit_{false};
	std::vector<std::thread> threads_;

	std::string message_{"test message"};

	//std::shared_ptr<Channel> channel_;

public:
	Bnch(const Bnch&) = delete;
	Bnch& operator=(const Bnch&) = delete;

	Bnch()
	//	: channel_(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()))
	{
		for (auto i = 0; i < FLAGS_workers; ++i)
		{
			threads_.emplace_back(&Bnch::main, this);
		}
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
		EchoClient client(
			grpc::CreateChannel(FLAGS_address, grpc::InsecureChannelCredentials())
		);
		//EchoClient client(channel_);

		while (!exit_)
		{
			client.echo(message_);
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
	gflags::SetUsageMessage("");
	gflags::ParseCommandLineFlags(&argc, &argv, true);

	gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
	Bnch bnch;

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
