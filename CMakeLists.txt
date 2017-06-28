cmake_minimum_required(VERSION 3.3)
project(grpc_echo)

find_package(Protobuf REQUIRED)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -g")

include_directories(/usr/local/include)
link_directories(/usr/local/lib)

set(PROTO_PATH "${CMAKE_SOURCE_DIR}/proto")
set(ECHO_PROTO "${PROTO_PATH}/echo.proto")
set(GENERATED_PROTOBUF_PATH "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_PROTOBUF_PATH})

set(ECHO_PB_CPP_FILE "${GENERATED_PROTOBUF_PATH}/echo.pb.cc")
set(ECHO_PB_H_FILE "${GENERATED_PROTOBUF_PATH}/echo.pb.h")
set(ECHO_GRPC_PB_CPP_FILE "${GENERATED_PROTOBUF_PATH}/echo.grpc.pb.cc")
set(ECHO_GRPC_PB_H_FILE "${GENERATED_PROTOBUF_PATH}/echo.grpc.pb.h")

add_custom_command(
    OUTPUT "${ECHO_PB_H_FILE}"
           "${ECHO_PB_CPP_FILE}"
    COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
    ARGS "--proto_path=${PROTO_PATH}"
         "--cpp_out=${GENERATED_PROTOBUF_PATH}"
         "${ECHO_PROTO}"
    )

add_custom_command(
    OUTPUT "${ECHO_GRPC_PB_H_FILE}"
           "${ECHO_GRPC_PB_CPP_FILE}"
    COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
    ARGS "--proto_path=${PROTO_PATH}"
         "--grpc_out=${GENERATED_PROTOBUF_PATH}"
         "--plugin=protoc-gen-grpc=/usr/local/bin/grpc_cpp_plugin"
         "${ECHO_PROTO}"
    )

set(GENERATED_PROTOBUF_FILES ${ECHO_PB_H_FILE} ${ECHO_PB_CPP_FILE}
    ${ECHO_GRPC_PB_H_FILE} ${ECHO_GRPC_PB_CPP_FILE})

include_directories(${GENERATED_PROTOBUF_PATH})
link_libraries(pthread grpc++_unsecure grpc gpr ${PROTOBUF_LIBRARY})

add_executable(echo_server echo_server.cc ${GENERATED_PROTOBUF_FILES})
add_executable(echo_server_async echo_server_async.cc ${GENERATED_PROTOBUF_FILES})
add_executable(echo_server_async2 echo_server_async2.cc ${GENERATED_PROTOBUF_FILES})
add_executable(echo_client echo_client.cc ${GENERATED_PROTOBUF_FILES})
add_executable(echo_client_async echo_client_async.cc ${GENERATED_PROTOBUF_FILES})
