// SPDX-License-Identifier: Apache-2.0
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sphinx/cluster_client.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class FakeServer final {
 public:
  using Handler = std::function<void(int)>;

  explicit FakeServer(Handler handler) : _handler{std::move(handler)} {
    _listener = socket(AF_INET, SOCK_STREAM, 0);
    if (_listener < 0) {
      throw std::runtime_error{"socket failed"};
    }
    int reuse = 1;
    if (setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
      throw std::runtime_error{"setsockopt failed"};
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      throw std::runtime_error{"bind failed"};
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(_listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
      throw std::runtime_error{"getsockname failed"};
    }
    _port = ntohs(address.sin_port);
    if (listen(_listener, 4) != 0) {
      throw std::runtime_error{"listen failed"};
    }
  }

  ~FakeServer() {
    if (_thread.joinable()) {
      _thread.join();
    }
    if (_listener >= 0) {
      close(_listener);
    }
  }

  FakeServer(const FakeServer&) = delete;
  FakeServer& operator=(const FakeServer&) = delete;

  uint16_t port() const { return _port; }

  void start() {
    _thread = std::thread{[this] {
      sockaddr_in address = {};
      socklen_t address_size = sizeof(address);
      const int client = accept(_listener, reinterpret_cast<sockaddr*>(&address), &address_size);
      if (client < 0) {
        return;
      }
      _handler(client);
      close(client);
    }};
  }

 private:
  int _listener = -1;
  uint16_t _port = 0;
  Handler _handler;
  std::thread _thread;
};

void send_chunks(int fd, std::string_view value);

class TwoConnectionServer final {
 public:
  TwoConnectionServer() {
    _listener = socket(AF_INET, SOCK_STREAM, 0);
    if (_listener < 0) {
      throw std::runtime_error{"socket failed"};
    }
    int reuse = 1;
    if (setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
      throw std::runtime_error{"setsockopt failed"};
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      throw std::runtime_error{"bind failed"};
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(_listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
      throw std::runtime_error{"getsockname failed"};
    }
    _port = ntohs(address.sin_port);
    if (listen(_listener, 2) != 0) {
      throw std::runtime_error{"listen failed"};
    }
  }

  ~TwoConnectionServer() {
    if (_thread.joinable()) {
      _thread.join();
    }
    if (_listener >= 0) {
      close(_listener);
    }
  }

  TwoConnectionServer(const TwoConnectionServer&) = delete;
  TwoConnectionServer& operator=(const TwoConnectionServer&) = delete;

  uint16_t port() const { return _port; }

  void start() {
    _thread = std::thread{[this] {
      for (int connection_index = 0; connection_index < 2; connection_index++) {
        sockaddr_in address = {};
        socklen_t address_size = sizeof(address);
        const int client = accept(_listener, reinterpret_cast<sockaddr*>(&address), &address_size);
        if (client < 0) {
          return;
        }
        char request[128];
        const auto count = recv(client, request, sizeof(request), 0);
        if (count <= 0) {
          close(client);
          return;
        }
        if (connection_index == 1) {
          send_chunks(client, "END\r\n");
        }
        close(client);
      }
    }};
  }

 private:
  int _listener = -1;
  uint16_t _port = 0;
  std::thread _thread;
};

bool read_until(int fd, std::string* buffer, std::string_view delimiter) {
  char chunk[4096];
  while (buffer->find(delimiter) == std::string::npos) {
    const auto count = recv(fd, chunk, sizeof(chunk), 0);
    if (count <= 0) {
      return false;
    }
    buffer->append(chunk, static_cast<size_t>(count));
  }
  return true;
}

void send_chunks(int fd, std::string_view value) {
  for (const auto chunk :
       {value.substr(0, value.size() / 3), value.substr(value.size() / 3, value.size() / 3),
        value.substr(value.size() / 3 * 2)}) {
    if (!chunk.empty()) {
      const auto count = send(fd, chunk.data(), chunk.size(), MSG_NOSIGNAL);
      if (count != static_cast<ssize_t>(chunk.size())) {
        throw std::runtime_error{"send failed"};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
  }
}

std::string read_request(int fd) {
  std::string request;
  if (!read_until(fd, &request, "\r\n")) {
    return {};
  }
  const auto header_end = request.find("\r\n");
  if (request.rfind("set ", 0) != 0) {
    return request;
  }
  const auto last_space = request.rfind(' ', header_end);
  if (last_space == std::string::npos) {
    throw std::runtime_error{"malformed set request"};
  }
  const auto bytes = std::stoull(request.substr(last_space + 1, header_end - last_space - 1));
  const auto complete_size = header_end + 2 + static_cast<size_t>(bytes) + 2;
  while (request.size() < complete_size) {
    char chunk[128];
    const auto count = recv(fd, chunk, sizeof(chunk), 0);
    if (count <= 0) {
      throw std::runtime_error{"client closed while sending request"};
    }
    request.append(chunk, static_cast<size_t>(count));
  }
  return request;
}

std::vector<std::string> parse_get_keys(const std::string& request) {
  if (request.rfind("get ", 0) != 0) {
    return {};
  }
  const auto line_end = request.find("\r\n");
  if (line_end == std::string::npos) {
    return {};
  }

  std::vector<std::string> keys;
  size_t position = 4;
  while (position < line_end) {
    const auto next_space = request.find(' ', position);
    const auto key_end =
        next_space == std::string::npos || next_space > line_end ? line_end : next_space;
    keys.emplace_back(request.substr(position, key_end - position));
    position = key_end + 1;
  }
  return keys;
}

std::string multi_get_response(const std::vector<std::string>& keys,
                               std::string_view missing_key = {}) {
  std::string response;
  for (auto key = keys.rbegin(); key != keys.rend(); ++key) {
    if (*key == missing_key) {
      continue;
    }
    const std::string value = "value:" + *key;
    response += "VALUE " + *key + " 0 " + std::to_string(value.size()) + "\r\n";
    response += value + "\r\n";
  }
  response += "END\r\n";
  return response;
}

std::string node_spec(uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

TEST(ClusterClientTest, HandlesPartialResponsesAndBinaryValues) {
  FakeServer server{[](int client) {
    const auto request = read_request(client);
    ASSERT_EQ(request.substr(0, request.find("\r\n") + 2), "set key 0 0 4\r\n");
    send_chunks(client, "STORED\r\n");

    std::string next_request;
    read_until(client, &next_request, "\r\n");
    const std::string value{"\0x\r\n", 4};
    std::string response{"VALUE key 0 4\r\n"};
    response += value;
    response += "\r\nEND\r\n";
    send_chunks(client, response);
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port())};
  const std::string binary_value{"\0x\r\n", 4};
  EXPECT_TRUE(client.set("key", binary_value));
  const auto value = client.get("key");
  ASSERT_TRUE(value.has_value());
  const std::string expected{"\0x\r\n", 4};
  EXPECT_EQ(value, std::optional{expected});
}

TEST(ClusterClientTest, SetSendsRelativeTtlWithoutChangingLegacyRequest) {
  FakeServer server{[](int client) {
    const auto request = read_request(client);
    EXPECT_EQ(request, "set product:v2:1 0 45 3\r\ntea\r\n");
    send_chunks(client, "STORED\r\n");
  }};
  server.start();
  sphinx::ClusterClient client{node_spec(server.port())};
  EXPECT_TRUE(client.set("product:v2:1", "tea", 45));
}

TEST(ClusterClientTest, RejectsAbsoluteTimestampRangeBeforeIo) {
  sphinx::ClusterClient client{"127.0.0.1:1"};
  EXPECT_THROW((void)client.set("key", "value", 60U * 60U * 24U * 30U + 1U), std::invalid_argument);
}

TEST(ClusterClientTest, GetMissReturnsNullopt) {
  FakeServer server{[](int client) {
    char request[128];
    ASSERT_GT(recv(client, request, sizeof(request), 0), 0);
    send_chunks(client, "END\r\n");
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port())};
  const auto result = client.get("missing");
  EXPECT_FALSE(result.has_value());
}

TEST(ClusterClientTest, MultiGetPreservesOrderAndParsesOutOfOrderBinaryValues) {
  std::mutex request_mutex;
  std::string received_request;
  const std::string binary_value{"A\0B", 3};
  FakeServer server{[&](int client) {
    const auto request = read_request(client);
    {
      std::lock_guard<std::mutex> lock{request_mutex};
      received_request = request;
    }
    std::string response{"VALUE binary 0 3\r\n"};
    response += binary_value;
    response += "\r\nVALUE first 0 5\r\nfirst\r\nEND\r\n";
    send_chunks(client, response);
  }};
  server.start();
  sphinx::ClusterClient client{node_spec(server.port())};
  const std::vector<std::string> keys{"first", "missing", "binary"};

  const auto values = client.get_many(keys);

  ASSERT_EQ(values.size(), 3U);
  EXPECT_EQ(values[0].value_or(""), "first");
  EXPECT_FALSE(values[1]);
  EXPECT_EQ(values[2].value_or(""), binary_value);
  {
    std::lock_guard<std::mutex> lock{request_mutex};
    EXPECT_EQ(received_request, "get first missing binary\r\n");
  }
}

TEST(ClusterClientTest, GroupsKeysByNodeAndRestoresDuplicateInputPositions) {
  std::mutex requests_mutex;
  std::vector<std::string> requests;
  const std::string missing_key{"multi-get-missing"};
  const auto handler = [&](int client) {
    const auto request = read_request(client);
    const auto keys = parse_get_keys(request);
    {
      std::lock_guard<std::mutex> lock{requests_mutex};
      requests.push_back(request);
    }
    send_chunks(client, multi_get_response(keys, missing_key));
  };
  FakeServer first_server{handler};
  FakeServer second_server{handler};
  first_server.start();
  second_server.start();
  const sphinx::Node first_node{"127.0.0.1", first_server.port()};
  const sphinx::Node second_node{"127.0.0.1", second_server.port()};
  sphinx::ClusterClient client{{first_node, second_node}};

  std::vector<std::string> first_node_keys;
  std::vector<std::string> second_node_keys;
  for (std::size_t index = 0;
       index < 10000 && (first_node_keys.size() < 2 || second_node_keys.size() < 2); ++index) {
    const std::string key = "multi-get-key-" + std::to_string(index);
    const sphinx::Node node = client.route(key);
    auto& node_keys = node == first_node ? first_node_keys : second_node_keys;
    if (node_keys.size() < 2) {
      node_keys.push_back(key);
    }
  }
  ASSERT_EQ(first_node_keys.size(), 2U);
  ASSERT_EQ(second_node_keys.size(), 2U);
  const std::vector<std::string> keys{first_node_keys[0], second_node_keys[0], missing_key,
                                      first_node_keys[1], second_node_keys[1], first_node_keys[0]};

  const auto values = client.get_many(keys);

  ASSERT_EQ(values.size(), keys.size());
  for (std::size_t index = 0; index < keys.size(); ++index) {
    if (keys[index] == missing_key) {
      EXPECT_FALSE(values[index]);
    } else {
      EXPECT_EQ(values[index].value_or(""), "value:" + keys[index]);
    }
  }

  const auto expected_request = [&](const sphinx::Node& target) {
    std::vector<std::string> node_keys;
    for (const auto& key : keys) {
      if (client.route(key) == target &&
          std::find(node_keys.begin(), node_keys.end(), key) == node_keys.end()) {
        node_keys.push_back(key);
      }
    }
    std::string request{"get"};
    for (const auto& key : node_keys) {
      request += " " + key;
    }
    return request + "\r\n";
  };
  std::vector<std::string> expected_requests{expected_request(first_node),
                                             expected_request(second_node)};
  std::sort(expected_requests.begin(), expected_requests.end());
  {
    std::lock_guard<std::mutex> lock{requests_mutex};
    std::sort(requests.begin(), requests.end());
    EXPECT_EQ(requests, expected_requests);
  }
}

TEST(ClusterClientTest, RejectsUnknownAndDuplicateMultiGetValues) {
  FakeServer unknown_key_server{[](int client) {
    char request[128];
    (void)recv(client, request, sizeof(request), 0);
    send_chunks(client, "VALUE unexpected 0 1\nx\r\nEND\r\n");
  }};
  unknown_key_server.start();
  sphinx::ClusterClient unknown_key_client{node_spec(unknown_key_server.port())};
  EXPECT_THROW((void)unknown_key_client.get_many({"requested"}), sphinx::ClientError);

  FakeServer duplicate_key_server{[](int client) {
    char request[128];
    (void)recv(client, request, sizeof(request), 0);
    send_chunks(client, "VALUE requested 0 1\nx\r\nVALUE requested 0 1\ny\r\nEND\r\n");
  }};
  duplicate_key_server.start();
  sphinx::ClusterClient duplicate_key_client{node_spec(duplicate_key_server.port())};
  EXPECT_THROW((void)duplicate_key_client.get_many({"requested"}), sphinx::ClientError);
}

TEST(ClusterClientTest, ResolvesHostnameForOperation) {
  FakeServer server{[](int client) {
    char request[128];
    ASSERT_GT(recv(client, request, sizeof(request), 0), 0);
    send_chunks(client, "END\r\n");
  }};
  server.start();
  sphinx::ClusterClient client{"localhost:" + std::to_string(server.port())};
  EXPECT_FALSE(client.get("missing").has_value());
}

TEST(ClusterClientTest, ConnectionReuseAvoidsASecondAccept) {
  std::atomic<int> accepts = 0;
  FakeServer server{[&](int client) {
    accepts.fetch_add(1);
    while (true) {
      auto request = read_request(client);
      if (request.rfind("set ", 0) == 0) {
        send_chunks(client, "STORED\r\n");
      } else if (request.rfind("get ", 0) == 0) {
        send_chunks(client, "VALUE key 0 3\r\nfoo\r\nEND\r\n");
      } else if (request.rfind("delete key", 0) == 0) {
        send_chunks(client, "DELETED\r\n");
      } else if (request.rfind("delete missing", 0) == 0) {
        send_chunks(client, "NOT_FOUND\r\n");
      } else {
        break;
      }
    }
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port())};
  EXPECT_TRUE(client.set("key", "bar"));
  EXPECT_EQ(client.get("key"), std::optional<std::string>{"foo"});
  EXPECT_TRUE(client.remove("key"));
  EXPECT_FALSE(client.remove("missing"));
  EXPECT_EQ(accepts.load(), 1);
}

TEST(ClusterClientTest, EarlyCloseIsAnErrorRatherThanAGetMiss) {
  FakeServer server{[](int client) {
    char ignored[64];
    (void)recv(client, ignored, sizeof(ignored), 0);
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port())};
  try {
    (void)client.get("key");
    FAIL() << "expected a client error";
  } catch (const sphinx::ClientError& error) {
    EXPECT_NE(std::string{error.what()}.find(node_spec(server.port())), std::string::npos);
  }
}

TEST(ClusterClientTest, MalformedResponseIsAnError) {
  for (const std::string_view response : {"NOT_A_MEMCACHED_RESPONSE\r\n", "VALUE key 0 \r\n"}) {
    SCOPED_TRACE(response);
    FakeServer server{[response](int client) {
      char ignored[64];
      (void)recv(client, ignored, sizeof(ignored), 0);
      send_chunks(client, response);
    }};
    server.start();

    sphinx::ClusterClient client{node_spec(server.port())};
    try {
      (void)client.get("key");
      FAIL() << "expected a client error";
    } catch (const sphinx::ClientError& error) {
      EXPECT_NE(std::string{error.what()}.find(node_spec(server.port())), std::string::npos);
    }
  }
}

TEST(ClusterClientTest, RejectsNonDecimalFlagsAndIncludesNode) {
  FakeServer server{[](int client) {
    char request[128];
    ASSERT_GT(recv(client, request, sizeof(request), 0), 0);
    send_chunks(client, "VALUE key not-decimal 3\r\nfoo\r\nEND\r\n");
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port())};
  try {
    (void)client.get("key");
    FAIL() << "expected a client error";
  } catch (const sphinx::ClientError& error) {
    EXPECT_NE(std::string{error.what()}.find(node_spec(server.port())), std::string::npos);
  }
}

TEST(ClusterClientTest, ReconnectsOnTheNextOperationAfterFailure) {
  TwoConnectionServer server;
  server.start();

  sphinx::ClusterClient client{node_spec(server.port()), std::chrono::milliseconds{200}};
  EXPECT_THROW((void)client.get("key"), sphinx::ClientError);
  EXPECT_FALSE(client.get("key").has_value());
}

TEST(ClusterClientTest, MultiGetReconnectsOnTheNextOperationAfterFailure) {
  TwoConnectionServer server;
  server.start();

  sphinx::ClusterClient client{node_spec(server.port()), std::chrono::milliseconds{200}};
  EXPECT_THROW((void)client.get_many({"first", "second"}), sphinx::ClientError);
  const auto values = client.get_many({"first", "second"});
  ASSERT_EQ(values.size(), 2U);
  EXPECT_FALSE(values[0]);
  EXPECT_FALSE(values[1]);
}

TEST(ClusterClientTest, DefaultTimeoutIsTwoSeconds) {
  EXPECT_EQ(sphinx::ClusterClient::kDefaultTimeout, std::chrono::milliseconds{2000});
}

TEST(ClusterClientTest, TimeoutIsBoundedAndIncludesNode) {
  FakeServer server{[](int client) {
    char ignored[64];
    (void)recv(client, ignored, sizeof(ignored), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port()), std::chrono::milliseconds{40}};
  const auto begin = std::chrono::steady_clock::now();
  try {
    (void)client.get("key");
    FAIL() << "expected a timeout";
  } catch (const sphinx::ClientError& error) {
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    EXPECT_NE(std::string{error.what()}.find(node_spec(server.port())), std::string::npos);
  }
}

TEST(ClusterClientTest, OneDeadlineCoversTrickledResponse) {
  FakeServer server{[](int client) {
    char ignored[64];
    (void)recv(client, ignored, sizeof(ignored), 0);
    constexpr std::string_view response = "VALUE key 0 1\r\nx\r\nEND\r\n";
    for (const char byte : response) {
      if (send(client, &byte, 1, MSG_NOSIGNAL) != 1) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port()), std::chrono::milliseconds{180}};
  const auto begin = std::chrono::steady_clock::now();
  EXPECT_THROW((void)client.get("key"), sphinx::ClientError);
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 450);
}

TEST(ClusterClientTest, RejectsOversizedValueBeforeAllocatingItsBody) {
  FakeServer server{[](int client) {
    char ignored[64];
    (void)recv(client, ignored, sizeof(ignored), 0);
    constexpr std::string_view header = "VALUE key 0 8388609\r\n";
    (void)send(client, header.data(), header.size(), MSG_NOSIGNAL);
  }};
  server.start();

  sphinx::ClusterClient client{node_spec(server.port())};
  try {
    (void)client.get("key");
    FAIL() << "oversized value was accepted";
  } catch (const sphinx::ClientError& error) {
    EXPECT_NE(std::string{error.what()}.find("value length exceeds client limit"),
              std::string::npos);
  }
}

}  // namespace
