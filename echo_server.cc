#include <iostream>
#include <memory>
#include <string>

#include <grpc++/grpc++.h>
#include <grpc/support/log.h>
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
		//std::cout << "recv: " << request->message() << std::endl;
		//gpr_log(GPR_DEBUG, "recv: %s", request->message().c_str());
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
	gpr_log(GPR_INFO, "Server listening on %s", server_address.c_str());
	//std::cout << "Server listening on " << server_address << std::endl;

	server->Wait();
}

int main(int argc, char** argv)
{
	gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
	runServer();
	return 0;
}
