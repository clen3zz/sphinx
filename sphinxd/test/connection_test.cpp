// SPDX-License-Identifier: Apache-2.0
#include "connection.h"

#include <gtest/gtest.h>
#include <sphinx/reactor-epoll.h>

#include <string>

TEST(ConnectionTest, rejectsExcessOutOfOrderResponseBytes) {
  auto group = std::make_shared<sphinx::ReactorGroup>(1);
  sphinx::EpollReactor reactor{0, group, [](const sphinx::MessagePtr&) {}};
  sphinx::Connection connection{1};
  std::string payload(sphinx::max_connection_response_bytes, 'x');
  EXPECT_EQ(connection.enqueue_response(1, payload, reactor),
            sphinx::Connection::WriteStatus::Complete);
  EXPECT_EQ(connection.enqueue_response(2, "x", reactor),
            sphinx::Connection::WriteStatus::ResourceLimit);
  connection.mark_closed();
  EXPECT_EQ(connection.enqueue_response(3, "x", reactor),
            sphinx::Connection::WriteStatus::SocketUnavailable);
}

TEST(ConnectionTest, multiGetOverflowProducesBoundedError) {
  sphinx::Connection connection{2};
  connection.begin_multi_get(0, 2);
  std::string payload(sphinx::max_connection_response_bytes / 2 + 1, 'x');
  EXPECT_FALSE(connection.add_multi_get_piece(0, 0, payload).has_value());
  const auto response = connection.add_multi_get_piece(0, 1, payload);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(*response, "SERVER_ERROR response too large\r\n");
  connection.begin_multi_get(1, 1);
  EXPECT_EQ(connection.add_multi_get_piece(1, 0, "VALUE a 0 1\r\nx\r\n"),
            "VALUE a 0 1\r\nx\r\nEND\r\n");
}
