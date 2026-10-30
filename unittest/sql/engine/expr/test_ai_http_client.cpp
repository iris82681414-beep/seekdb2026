// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace {
// Each connection returns a retryable HTTP failure. Its ephemeral loopback
// port avoids an external service or fixed-port dependency for this test.
class UnavailableServer {
public:
  explicit UnavailableServer(int status = 503)
      : fd_(socket(AF_INET, SOCK_STREAM, 0)), port_(0), stop_(false), requests_(0), status_(status)
  {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(fd_, 8) != 0) {
      return;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
      return;
    }
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this]() {
      while (!stop_) {
        const int client = accept(fd_, nullptr, nullptr);
        if (client < 0) { break; }
        timeval timeout{2, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::string request;
        char buffer[4096];
        while (true) {
          const ssize_t size = recv(client, buffer, sizeof(buffer), 0);
          if (size <= 0) { break; }
          request.append(buffer, size);
          const size_t header_end = request.find("\r\n\r\n");
          const size_t field = request.find("Content-Length:");
          if (header_end != std::string::npos) {
            const size_t body_size = field == std::string::npos ? 0 :
                std::stoul(request.substr(field + 15));
            if (request.size() >= header_end + 4 + body_size) { break; }
          }
        }
        ++requests_;
        const std::string response = "HTTP/1.1 " + std::to_string(status_) +
            " Unavailable\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        send(client, response.data(), response.size(), MSG_NOSIGNAL);
        close(client);
      }
    });
  }
  ~UnavailableServer()
  {
    stop_ = true;
    shutdown(fd_, SHUT_RDWR);
    if (worker_.joinable()) { worker_.join(); }
    if (fd_ >= 0) { close(fd_); }
  }
  uint16_t port() const { return port_; }
  int requests() const { return requests_.load(); }
private:
  int fd_;
  uint16_t port_;
  std::atomic<bool> stop_;
  std::atomic<int> requests_;
  int status_;
  std::thread worker_;
};

TEST(AIHttpClient, batch_retry_exhaustion_returns_error)
{
  UnavailableServer server;
  ASSERT_NE(0, server.port());
  ObArenaAllocator allocator;
  ObAIFuncClient client;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator, body));
  ObArray<ObJsonObject *> requests;
  ObArray<ObJsonObject *> responses;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, requests.push_back(body));
  ASSERT_EQ(OB_SUCCESS, requests.push_back(body));
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  const std::string address = "http://127.0.0.1:" + std::to_string(server.port());
  ObString url(address.size(), address.data());
  EXPECT_EQ(OB_CURL_ERROR, client.send_post_batch(allocator, url, headers, requests, responses));
  EXPECT_EQ(0, responses.count());
}

TEST(AIHttpClient, batch_without_retries_still_sends_initial_requests)
{
  UnavailableServer server;
  ASSERT_NE(0, server.port());
  ObArenaAllocator allocator;
  ObAIFuncClient client(false /* enable_retry */);
  ObJsonObject *body = nullptr;
  ObArray<ObJsonObject *> requests;
  ObArray<ObJsonObject *> responses;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator, body));
  ASSERT_EQ(OB_SUCCESS, requests.push_back(body));
  ASSERT_EQ(OB_SUCCESS, requests.push_back(body));
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  const std::string address = "http://127.0.0.1:" + std::to_string(server.port());
  ObString url(address.size(), address.data());
  EXPECT_EQ(OB_CURL_ERROR, client.send_post_batch(allocator, url, headers, requests, responses));
  EXPECT_EQ(0, responses.count());
  EXPECT_EQ(2, server.requests());
}

TEST(AIHttpClient, scalar_default_retries_are_preserved)
{
  UnavailableServer server;
  ASSERT_NE(0, server.port());
  ObArenaAllocator allocator;
  ObAIFuncClient client;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator, body));
  ASSERT_EQ(OB_SUCCESS, headers.push_back(ObString("Content-Type: application/json")));
  const std::string address = "http://127.0.0.1:" + std::to_string(server.port());
  ObString url(address.size(), address.data());
  EXPECT_EQ(OB_CURL_ERROR, client.send_post(allocator, url, headers, body, response));
  EXPECT_EQ(nullptr, response);
  EXPECT_EQ(4, server.requests());
}

TEST(AIFuncModel, queue_owned_retries_issue_one_request_per_dispatch)
{
  for (const int status : {429, 503}) {
    for (const bool embedding : {false, true}) {
      UnavailableServer server(status);
      ASSERT_NE(0, server.port());
      ObArenaAllocator allocator;
      ObAIFuncExprInfo info(allocator, embedding ? T_FUN_SYS_AI_EMBED : T_FUN_SYS_AI_COMPLETE);
      info.type_ = embedding ? oceanbase::share::EndpointType::DENSE_EMBEDDING :
          oceanbase::share::EndpointType::COMPLETION;
      info.model_ = "mock-model";
      const std::string options = "{\"ai_model_name\":\"mock-model\",\"provider\":\"OPENAI\","
          "\"access_key\":\"local-mock-key\",\"url\":\"http://127.0.0.1:" +
          std::to_string(server.port()) + "\"}";
      ObString json(options.size(), options.data());
      ObJsonObject *params = nullptr;
      ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, params));
      oceanbase::share::ObAiModelEndpointInfo endpoint;
      ASSERT_EQ(OB_SUCCESS, endpoint.parse_from_json_base(allocator, ObString("mock-endpoint"), *params));
      ObAIFuncModel model(allocator, info, endpoint, false /* enable_retry */);
      ObString input("mock input");
      for (int dispatch = 1; dispatch <= 2; ++dispatch) {
        ObString result;
        const int ret = embedding ? model.call_dense_embedding(input, nullptr, result) :
            model.call_completion(input, nullptr, result);
        EXPECT_EQ(OB_CURL_ERROR, ret) << status << ": embedding=" << embedding;
        EXPECT_TRUE(result.empty());
        EXPECT_EQ(dispatch, server.requests());
      }
    }
  }
}
} // namespace
