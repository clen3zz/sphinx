/*
Copyright 2018 The Sphinxd Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <gtest/gtest.h>

#include <sphinx/reactor-epoll.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>
namespace {

class TestReactor final : public sphinx::reactor::EpollReactor
{
public:
  TestReactor(size_t thread_id, size_t nr_threads, sphinx::reactor::OnMessageFn&& on_message_fn)
    : EpollReactor{thread_id, nr_threads, std::move(on_message_fn)}
  {
  }

  using sphinx::reactor::Reactor::poll_messages;
};

} // namespace

TEST(ReactorTest, messageCanBeQueuedBeforeRemoteReactorStarts)
{
  size_t received = 0;
  TestReactor source{0, 2, [](void* message) { delete static_cast<int*>(message); }};
  ASSERT_TRUE(source.send_msg(1, new int{1}));
  TestReactor target{1, 2, [&received](void* message) {
                       received++;
                       delete static_cast<int*>(message);
                     }};
  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received, 1U);
}

TEST(ReactorTest, fullBoundedQueueReturnsBackpressureAndDrains)
{
  size_t received = 0;
  TestReactor source{0, 2, [](void* message) { delete static_cast<int*>(message); }};
  TestReactor target{1, 2, [&received](void* message) {
                       received++;
                       delete static_cast<int*>(message);
                     }};

  size_t sent = 0;
  bool rejected = false;
  for (;;) {
    auto* message = new int{static_cast<int>(sent)};
    if (!source.send_msg(1, message)) {
      delete message;
      rejected = true;
      break;
    }
    sent++;
  }
  ASSERT_TRUE(rejected);
  ASSERT_EQ(sent, 9999U); // Queue capacity is N-1 by design.
  auto* deferred_message = new int{0};
  ASSERT_TRUE(source.send_msg_deferred(1, deferred_message));

  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received, sent + 1);
  auto* final_message = new int{0};
  ASSERT_TRUE(source.send_msg(1, final_message));
  ASSERT_TRUE(target.poll_messages());
  ASSERT_EQ(received, sent + 2);
}

TEST(ReactorTest, tcpSocketDrainsPartialNonblockingWrites)
{
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
  socket->on_pollin(); // A nonblocking read with no data is not a connection error.
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
