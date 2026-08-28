// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/reactor-epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>
namespace {

struct IntMessage final : sphinx::reactor::Message {
  explicit IntMessage(int initial_value) : value{initial_value} {
  }

  int value;
};

class TestReactor final : public sphinx::reactor::EpollReactor {
 public:
  TestReactor(size_t thread_id, std::shared_ptr<sphinx::reactor::ReactorGroup> group,
              sphinx::reactor::OnMessageFn&& on_message_fn)
      : EpollReactor{thread_id, std::move(group), std::move(on_message_fn)} {
  }

  using sphinx::reactor::Reactor::poll_messages;
};

}  // namespace

TEST(ReactorTest, messageCanBeQueuedBeforeRemoteReactorStarts) {
  size_t received = 0;
  auto group = std::make_shared<sphinx::reactor::ReactorGroup>(2);
  TestReactor source{0, group, [](sphinx::reactor::MessagePtr) {}};
  ASSERT_TRUE(source.send_msg(1, std::make_shared<IntMessage>(1)));
  TestReactor target{1, group, [&received](sphinx::reactor::MessagePtr message) {
                       received++;
                       ASSERT_EQ(std::dynamic_pointer_cast<IntMessage>(message)->value, 1);
                     }};
  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received, 1U);
}

TEST(ReactorTest, fullBoundedQueueReturnsBackpressureAndDrains) {
  size_t received = 0;
  auto group = std::make_shared<sphinx::reactor::ReactorGroup>(2);
  TestReactor source{0, group, [](sphinx::reactor::MessagePtr) {}};
  TestReactor target{1, group, [&received](sphinx::reactor::MessagePtr) { received++; }};

  size_t sent = 0;
  bool rejected = false;
  for (;;) {
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

TEST(ReactorTest, groupsOwnIndependentMessageChannels) {
  size_t received = 0;
  auto first_group = std::make_shared<sphinx::reactor::ReactorGroup>(2);
  auto second_group = std::make_shared<sphinx::reactor::ReactorGroup>(2);
  TestReactor first_source{0, first_group, [](sphinx::reactor::MessagePtr) {}};
  TestReactor first_target{1, first_group,
                           [&received](sphinx::reactor::MessagePtr) { received++; }};
  TestReactor second_target{1, second_group,
                            [&received](sphinx::reactor::MessagePtr) { received += 100; }};

  ASSERT_TRUE(first_source.send_msg(1, std::make_shared<IntMessage>(1)));
  ASSERT_TRUE(first_target.poll_messages());
  ASSERT_FALSE(second_target.poll_messages());
  ASSERT_EQ(received, 1U);
}

TEST(ReactorTest, tcpSocketDrainsPartialNonblockingWrites) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
  int send_buffer_size = 1024;
  ASSERT_EQ(
      ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)), 0);
  bool eof = false;
  auto socket = std::make_shared<sphinx::reactor::TcpSocket>(
      fds[0], [&eof](std::shared_ptr<sphinx::reactor::TcpSocket>, std::string_view msg) {
        eof = msg.empty();
      });
  socket->on_pollin();  // 非阻塞读取暂无数据时不属于连接错误。
  ASSERT_FALSE(eof);
  std::string payload(1024 * 1024, 'x');
  ASSERT_FALSE(socket->send(payload.data(), payload.size()));

  std::string received;
  received.reserve(payload.size());
  for (size_t attempt = 0; attempt < 10000 && received.size() < payload.size(); attempt++) {
    char buf[8192];
    for (;;) {
      auto nr = ::recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT);
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

  ::close(fds[1]);
  socket->on_pollin();
  ASSERT_TRUE(eof);
}
