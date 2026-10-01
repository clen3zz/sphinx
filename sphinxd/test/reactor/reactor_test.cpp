// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/reactor-epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
namespace {

struct IntMessage final : sphinx::Message {
  explicit IntMessage(int initial_value) : value{initial_value} {}

  int value;
};

struct LargeMessage final : sphinx::Message {
  explicit LargeMessage(size_t size) : payload(size, 'x') {}
  size_t queued_bytes() const noexcept override { return sizeof(LargeMessage) + payload.size(); }
  std::string payload;
};

class TestReactor final : public sphinx::EpollReactor {
 public:
  TestReactor(size_t thread_id, std::shared_ptr<sphinx::ReactorGroup> group,
              sphinx::OnMessageFn&& on_message_fn)
      : EpollReactor{thread_id, std::move(group), std::move(on_message_fn)} {}

  using EpollReactor::poll_messages;
};

}  // namespace

TEST(ReactorTest, RejectsInvalidInputsBeforeChannelAccess) {
  EXPECT_THROW(sphinx::ReactorGroup{0}, std::invalid_argument);
  EXPECT_THROW(sphinx::ReactorGroup{sphinx::max_nr_threads + 1}, std::invalid_argument);
  auto group = std::make_shared<sphinx::ReactorGroup>(2);
  const auto ignore_message = [](const sphinx::MessagePtr&) {};
  EXPECT_THROW((TestReactor{0, nullptr, ignore_message}), std::invalid_argument);
  EXPECT_THROW((TestReactor{2, group, ignore_message}), std::invalid_argument);
  TestReactor source{0, group, ignore_message};
  auto message = std::make_shared<IntMessage>(1);
  for (const size_t target : {size_t{0}, size_t{2}}) {
    EXPECT_THROW(source.send_msg(target, message), std::invalid_argument);
    EXPECT_THROW(source.send_msg_deferred(target, message), std::invalid_argument);
    EXPECT_THROW(source.notify_overload(target, 42), std::invalid_argument);
  }
  EXPECT_THROW(source.send_msg(1, nullptr), std::invalid_argument);
  EXPECT_THROW(source.send_msg_deferred(1, nullptr), std::invalid_argument);
  EXPECT_THROW(source.notify_overload(1, 0), std::invalid_argument);
  EXPECT_TRUE(source.send_msg(1, message));
}

TEST(ReactorTest, messageCanBeQueuedBeforeRemoteReactorStarts) {
  size_t received = 0;
  auto group = std::make_shared<sphinx::ReactorGroup>(2);
  TestReactor source{0, group, [](const sphinx::MessagePtr&) {}};
  ASSERT_TRUE(source.send_msg(1, std::make_shared<IntMessage>(1)));
  TestReactor target{1, group, [&received](const sphinx::MessagePtr& message) {
                       received++;
                       ASSERT_EQ(std::dynamic_pointer_cast<IntMessage>(message)->value, 1);
                     }};
  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received, 1U);
}

TEST(ReactorTest, fullBoundedQueueReturnsBackpressureAndDrains) {
  size_t received = 0;
  auto group = std::make_shared<sphinx::ReactorGroup>(2);
  TestReactor source{0, group, [](const sphinx::MessagePtr&) {}};
  TestReactor target{1, group, [&received](const sphinx::MessagePtr&) { received++; }};

  size_t sent = 0;
  bool rejected = false;
  while (true) {
    auto message = std::make_shared<IntMessage>(static_cast<int>(sent));
    if (!source.send_msg(1, message)) {
      rejected = true;
      break;
    }
    sent++;
  }
  ASSERT_TRUE(rejected);
  ASSERT_EQ(sent, 9999U);  // 队列容量按设计为 N-1。
  auto deferred_message = std::make_shared<IntMessage>(0);
  ASSERT_TRUE(source.send_msg_deferred(1, deferred_message));

  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received, sent + 1);
  auto final_message = std::make_shared<IntMessage>(0);
  ASSERT_TRUE(source.send_msg(1, final_message));
  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received, sent + 2);
}

TEST(ReactorTest, deferredMessagesKeepOrderAfterRingFills) {
  std::vector<int> received;
  auto group = std::make_shared<sphinx::ReactorGroup>(2);
  TestReactor source{0, group, [](const sphinx::MessagePtr&) {}};
  TestReactor target{1, group, [&received](const sphinx::MessagePtr& message) {
                       received.push_back(std::dynamic_pointer_cast<IntMessage>(message)->value);
                     }};

  for (int value = 0; value < 10002; ++value) {
    ASSERT_TRUE(source.send_msg_deferred(1, std::make_shared<IntMessage>(value)));
  }
  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received.size(), 10002U);
  for (int value = 0; value < 10002; ++value) {
    EXPECT_EQ(received[static_cast<size_t>(value)], value);
  }
}

TEST(ReactorTest, groupsOwnIndependentMessageChannels) {
  size_t received = 0;
  auto first_group = std::make_shared<sphinx::ReactorGroup>(2);
  auto second_group = std::make_shared<sphinx::ReactorGroup>(2);
  TestReactor first_source{0, first_group, [](const sphinx::MessagePtr&) {}};
  TestReactor first_target{1, first_group, [&received](const sphinx::MessagePtr&) { received++; }};
  TestReactor second_target{1, second_group,
                            [&received](const sphinx::MessagePtr&) { received += 100; }};

  ASSERT_TRUE(first_source.send_msg(1, std::make_shared<IntMessage>(1)));
  ASSERT_TRUE(first_target.poll_messages());
  ASSERT_FALSE(second_target.poll_messages());
  ASSERT_EQ(received, 1U);
}

TEST(ReactorTest, ChannelRejectsExcessBytesAndDeliversOverloadNotice) {
  size_t received = 0;
  bool overloaded = false;
  auto group = std::make_shared<sphinx::ReactorGroup>(2);
  TestReactor source{0, group, [](const sphinx::MessagePtr&) {}};
  TestReactor target{
      1, group, [&](const sphinx::MessagePtr& message) {
        if (auto notice = std::dynamic_pointer_cast<sphinx::ReactorOverload>(message)) {
          overloaded = notice->connection_id == 42 && !notice->close_all;
        } else {
          received++;
        }
      }};
  auto large = std::make_shared<LargeMessage>(size_t{9} * 1024 * 1024);
  ASSERT_TRUE(source.send_msg_deferred(1, large));
  ASSERT_FALSE(source.send_msg_deferred(1, large));
  source.notify_overload(1, 42);
  ASSERT_TRUE(target.poll_messages());
  EXPECT_TRUE(overloaded);
  EXPECT_EQ(received, 1U);
  EXPECT_TRUE(source.send_msg_deferred(1, large));
  EXPECT_TRUE(target.poll_messages());
}

TEST(ReactorTest, GroupBoundsBytesAcrossIndependentChannels) {
  auto group = std::make_shared<sphinx::ReactorGroup>(9);
  std::vector<std::unique_ptr<TestReactor>> sources;
  auto large = std::make_shared<LargeMessage>(size_t{9} * 1024 * 1024);
  for (size_t id = 1; id <= 8; ++id) {
    sources.push_back(std::make_unique<TestReactor>(id, group, [](const sphinx::MessagePtr&) {}));
  }
  for (size_t id = 0; id < 7; ++id) {
    EXPECT_TRUE(sources[id]->send_msg_deferred(0, large));
  }
  EXPECT_FALSE(sources[7]->send_msg_deferred(0, large));
  size_t received = 0;
  TestReactor target{0, group, [&](const sphinx::MessagePtr&) { ++received; }};
  EXPECT_TRUE(target.poll_messages());
  EXPECT_EQ(received, 7U);
  EXPECT_TRUE(sources[7]->send_msg_deferred(0, large));
}

TEST(ReactorTest, TcpSocketClosesWhenUnsentBytesExceedLimit) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
  int send_buffer_size = 1024;
  ASSERT_EQ(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)),
            0);
  auto socket = std::make_shared<sphinx::TcpSocket>(
      fds[0], [](const std::shared_ptr<sphinx::TcpSocket>&, std::string_view) {});
  std::string payload(sphinx::max_connection_response_bytes, 'x');
  ASSERT_FALSE(socket->send(payload.data(), payload.size()));
  EXPECT_TRUE(socket->send(payload.data(), payload.size()));
  EXPECT_TRUE(socket->closed());
  close(fds[1]);
}

TEST(ReactorTest, tcpSocketDrainsPartialNonblockingWrites) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
  int send_buffer_size = 1024;
  ASSERT_EQ(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)),
            0);
  bool eof = false;
  auto socket = std::make_shared<sphinx::TcpSocket>(
      fds[0], [&eof](const std::shared_ptr<sphinx::TcpSocket>&, std::string_view msg) {
        eof = msg.empty();
      });
  socket->on_pollin();  // 非阻塞读取暂无数据时不属于连接错误。
  ASSERT_FALSE(eof);
  std::string payload(size_t{1024} * 1024, 'x');
  ASSERT_FALSE(socket->send(payload.data(), payload.size()));

  std::string received;
  received.reserve(payload.size());
  for (size_t attempt = 0; attempt < 10000 && received.size() < payload.size(); attempt++) {
    char buf[8192];
    while (true) {
      auto nr = recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT);
      if (nr > 0) {
        received.append(buf, static_cast<size_t>(nr));
        continue;
      }
      if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      ASSERT_GE(nr, 0);
      break;
    }
    if (received.size() < payload.size()) {
      socket->on_pollout();
    }
  }
  ASSERT_EQ(received, payload);

  close(fds[1]);
  socket->on_pollin();
  ASSERT_TRUE(eof);
}
