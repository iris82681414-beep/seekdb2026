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
  UnavailableServer() : fd_(socket(AF_INET, SOCK_STREAM, 0)), port_(0), stop_(false)
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
        const char response[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
        send(client, response, sizeof(response) - 1, MSG_NOSIGNAL);
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
private:
  int fd_;
  uint16_t port_;
  std::atomic<bool> stop_;
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
} // namespace
