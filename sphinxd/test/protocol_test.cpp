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

#include <sphinx/protocol.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

TEST(ProtocolTest, parse_error)
{
  using namespace sphinx::memcache;
  std::string msg = "foo";
  Parser parser;
  parser.parse(msg);
  ASSERT_EQ(bool(parser._op), false);
}

TEST(ProtocolTest, parse_set)
{
  using namespace sphinx::memcache;
  std::string msg = "set foo 0 0 3\r\nbar\r\n";
  Parser parser;
  parser.parse(msg);
  ASSERT_EQ(*parser._op, Opcode::Set);
}

TEST(ProtocolTest, parse_get)
{
  using namespace sphinx::memcache;
  std::string msg = "get foo\r\n";
  Parser parser;
  parser.parse(msg);
  ASSERT_EQ(*parser._op, Opcode::Get);
}

TEST(ProtocolTest, parse_many)
{
  using namespace sphinx::memcache;
  std::string raw_msg = "set foo 0 0 3\r\nbar\r\nget foo\r\n";
  std::string_view msg = raw_msg;
  {
    Parser parser;
    auto nr_consumed = parser.parse(msg);
    ASSERT_EQ(15, nr_consumed);
    ASSERT_EQ(parser._op, Opcode::Set);
    ASSERT_EQ(3, parser._blob_size);
    msg.remove_prefix(nr_consumed + parser._blob_size + 2);
  }
  {
    Parser parser;
    auto nr_consumed = parser.parse(msg);
    ASSERT_EQ(9, nr_consumed);
    ASSERT_EQ(*parser._op, Opcode::Get);
  }
}

TEST(ProtocolTest, parse_pipelined_headers_without_consuming_next_command)
{
  using namespace sphinx::memcache;
  std::string_view msg = "get first\r\nget second\r\n";
  Parser first;
  auto first_consumed = first.parse(msg);
  ASSERT_EQ(first_consumed, 11U);
  ASSERT_EQ(first._op, Opcode::Get);
  ASSERT_EQ(first.key(), "first");

  msg.remove_prefix(first_consumed);
  Parser second;
  auto second_consumed = second.parse(msg);
  ASSERT_EQ(second_consumed, 12U);
  ASSERT_EQ(second._op, Opcode::Get);
  ASSERT_EQ(second.key(), "second");
}

TEST(ProtocolTest, parse_multi_get_preserves_order_and_owns_keys)
{
  using namespace sphinx::memcache;
  std::string msg = "get first second first\r\nget after\r\n";
  Parser parser;
  auto nr_consumed = parser.parse(msg);

  ASSERT_EQ(nr_consumed, 24U);
  ASSERT_EQ(parser._op, Opcode::Get);
  ASSERT_EQ(parser.keys(), (std::vector<std::string>{"first", "second", "first"}));
  ASSERT_EQ(parser.key(), "first");

  // The key results are owned by Parser, rather than views into the receive
  // buffer that the reactor is free to move or release after parsing.
  msg.clear();
  ASSERT_EQ(parser.keys()[1], "second");
  ASSERT_EQ(parser.keys()[2], "first");
}

TEST(ProtocolTest, parse_delete)
{
  using namespace sphinx::memcache;
  Parser parser;
  auto msg = std::string{"delete gone\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  ASSERT_EQ(parser._op, Opcode::Delete);
  ASSERT_EQ(parser.key(), "gone");
}

TEST(ProtocolTest, parse_incr_and_decr_delta)
{
  using namespace sphinx::memcache;
  {
    Parser parser;
    auto msg = std::string{"incr counter 0\r\n"};
    ASSERT_EQ(parser.parse(msg), msg.size());
    ASSERT_EQ(parser._op, Opcode::Incr);
    ASSERT_EQ(parser.key(), "counter");
    ASSERT_EQ(parser.delta(), 0U);
  }
  {
    Parser parser;
    auto msg = std::string{"decr counter 18446744073709551615\r\n"};
    ASSERT_EQ(parser.parse(msg), msg.size());
    ASSERT_EQ(parser._op, Opcode::Decr);
    ASSERT_EQ(parser.delta(), std::numeric_limits<uint64_t>::max());
    ASSERT_FALSE(parser._number_overflow);
  }
}

TEST(ProtocolTest, parse_delta_overflow_is_reported_without_losing_opcode)
{
  using namespace sphinx::memcache;
  Parser parser;
  auto msg = std::string{"incr counter 18446744073709551616\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  ASSERT_EQ(parser._op, Opcode::Incr);
  ASSERT_TRUE(parser._number_overflow);
}

TEST(ProtocolTest, incomplete_header_requests_more_data)
{
  using namespace sphinx::memcache;
  for (const auto& msg :
       {std::string{"get first"}, std::string{"delete first\r"}, std::string{"incr first 1"}}) {
    Parser parser;
    ASSERT_EQ(parser.parse(msg), 0U);
    ASSERT_FALSE(parser._op);
  }
}

TEST(ProtocolTest, parse_mixed_pipeline_one_header_at_a_time)
{
  using namespace sphinx::memcache;
  std::string_view msg = "get first second\r\ndelete old\r\nincr count 7\r\ndecr count 2\r\n";

  Parser get;
  auto get_consumed = get.parse(msg);
  ASSERT_EQ(get_consumed, 18U);
  ASSERT_EQ(get._op, Opcode::Get);
  ASSERT_EQ(get.keys(), (std::vector<std::string>{"first", "second"}));
  msg.remove_prefix(get_consumed);

  Parser remove;
  auto remove_consumed = remove.parse(msg);
  ASSERT_EQ(remove_consumed, 12U);
  ASSERT_EQ(remove._op, Opcode::Delete);
  ASSERT_EQ(remove.key(), "old");
  msg.remove_prefix(remove_consumed);

  Parser incr;
  auto incr_consumed = incr.parse(msg);
  ASSERT_EQ(incr_consumed, 14U);
  ASSERT_EQ(incr._op, Opcode::Incr);
  ASSERT_EQ(incr.delta(), 7U);
  msg.remove_prefix(incr_consumed);

  Parser decr;
  ASSERT_EQ(decr.parse(msg), msg.find("\r\n") + 2);
  ASSERT_EQ(decr._op, Opcode::Decr);
  ASSERT_EQ(decr.delta(), 2U);
}

TEST(ProtocolTest, invalid_complete_command_does_not_consume_next_command)
{
  using namespace sphinx::memcache;
  std::string_view msg = "delete first second\r\nget valid\r\n";

  Parser invalid;
  auto invalid_consumed = invalid.parse(msg);
  ASSERT_EQ(invalid_consumed, 21U);
  ASSERT_FALSE(invalid._op);
  msg.remove_prefix(invalid_consumed);

  Parser valid;
  ASSERT_EQ(valid.parse(msg), 11U);
  ASSERT_EQ(valid._op, Opcode::Get);
  ASSERT_EQ(valid.key(), "valid");
}

TEST(ProtocolTest, parse_stats)
{
  using namespace sphinx::memcache;
  Parser parser;
  auto msg = std::string{"stats\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  ASSERT_EQ(parser._op, Opcode::Stats);
  ASSERT_TRUE(parser.keys().empty());
}

TEST(ProtocolTest, parse_stats_header_in_fragments)
{
  using namespace sphinx::memcache;
  Parser parser;
  ASSERT_EQ(parser.parse("sta"), 0U);
  ASSERT_FALSE(parser._op);
  ASSERT_EQ(parser.parse("stats\r"), 0U);
  ASSERT_FALSE(parser._op);

  auto complete = std::string{"stats\r\n"};
  ASSERT_EQ(parser.parse(complete), complete.size());
  ASSERT_EQ(parser._op, Opcode::Stats);
}

TEST(ProtocolTest, parse_stats_in_pipeline)
{
  using namespace sphinx::memcache;
  std::string_view msg = "version\r\nstats\r\nget key\r\n";

  Parser version;
  auto version_consumed = version.parse(msg);
  ASSERT_EQ(version_consumed, 9U);
  ASSERT_EQ(version._op, Opcode::Version);
  msg.remove_prefix(version_consumed);

  Parser stats;
  auto stats_consumed = stats.parse(msg);
  ASSERT_EQ(stats_consumed, 7U);
  ASSERT_EQ(stats._op, Opcode::Stats);
  ASSERT_TRUE(stats.keys().empty());
  msg.remove_prefix(stats_consumed);

  Parser get;
  ASSERT_EQ(get.parse(msg), 9U);
  ASSERT_EQ(get._op, Opcode::Get);
  ASSERT_EQ(get.key(), "key");
}
