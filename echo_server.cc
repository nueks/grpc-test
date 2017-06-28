#include <iostream>
#include <memory>
#include <string>

#include <grpc++/grpc++.h>
#include "echo.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

using echo::EchoRequest;
using echo::EchoResponse;
using echo::Echo;


class EchoService final : public Echo::Service {
	Status Process(ServerContext* context, const EchoRequest* request, EchoResponse* response) override
	{
		response->set_message(request->message());
		return Status::OK;
	}
};

void runServer()
{
	std::string server_address("0.0.0.0:50051");
	EchoService service;

	ServerBuilder builder;
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);

	std::unique_ptr<Server> server(builder.BuildAndStart());
	std::cout << "Server listening on " << server_address << std::endl;

	server->Wait();
}

int main(int argc, char** argv)
{
	runServer();
	return 0;
}
