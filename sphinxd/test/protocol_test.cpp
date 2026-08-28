// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/protocol.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

template <typename Command>
const Command* command_as(const sphinx::memcache::Parser& parser) {
  const auto& parsed = parser.command();
  if (!parsed) {
    return nullptr;
  }
  return std::get_if<Command>(&parsed.value());
}

template <typename Command>
bool has_command(const sphinx::memcache::Parser& parser) {
  const auto& parsed = parser.command();
  if (!parsed) {
    return false;
  }
  return std::holds_alternative<Command>(parsed.value());
}

}  // namespace

TEST(ProtocolTest, parse_error) {
  using namespace sphinx::memcache;
  std::string msg = "foo";
  Parser parser;
  parser.parse(msg);
  ASSERT_EQ(parser.status(), ParseStatus::Incomplete);
  ASSERT_FALSE(parser.command().has_value());
}

TEST(ProtocolTest, parse_set) {
  using namespace sphinx::memcache;
  std::string msg = "set foo 0 0 3\r\nbar\r\n";
  Parser parser;
  parser.parse(msg);
  ASSERT_TRUE(parser.command().has_value());
  ASSERT_TRUE(has_command<SetCommand>(parser));
}

TEST(ProtocolTest, parsed_command_is_typed_and_describes_storage_body) {
  using namespace sphinx::memcache;
  std::string msg = "set foo 7 11 3\r\nbar\r\n";
  Parser parser;
  const auto header_size = parser.parse(msg);

  ASSERT_EQ(parser.status(), ParseStatus::Parsed);
  ASSERT_TRUE(parser.command().has_value());
  const auto* command = command_as<SetCommand>(parser);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->key, "foo");
  EXPECT_EQ(command->flags, 7U);
  EXPECT_EQ(command->expiration, 11U);
  EXPECT_EQ(command->body.size, 3U);
  EXPECT_EQ(command->body.offset, header_size);
  ASSERT_TRUE(command->body.available(msg));
  ASSERT_TRUE(command->body.has_valid_terminator(msg));
  const auto value = command->body.view(msg);
  ASSERT_TRUE(value.has_value());
  if (!value) {
    return;
  }
  EXPECT_EQ(*value, "bar");
}

TEST(ProtocolTest, parsed_command_owns_get_keys) {
  using namespace sphinx::memcache;
  std::string msg = "get first second first\r\n";
  Parser parser;
  parser.parse(msg);

  ASSERT_TRUE(parser.command().has_value());
  const auto* command = command_as<GetCommand>(parser);
  ASSERT_NE(command, nullptr);
  ASSERT_EQ(command->keys, (std::vector<std::string>{"first", "second", "first"}));
  msg.clear();
  EXPECT_EQ(command->keys[1], "second");
}

TEST(ProtocolTest, parse_get) {
  using namespace sphinx::memcache;
  std::string msg = "get foo\r\n";
  Parser parser;
  parser.parse(msg);
  ASSERT_TRUE(parser.command().has_value());
  ASSERT_TRUE(has_command<GetCommand>(parser));
}

TEST(ProtocolTest, parse_many) {
  using namespace sphinx::memcache;
  std::string raw_msg = "set foo 0 0 3\r\nbar\r\nget foo\r\n";
  std::string_view msg = raw_msg;
  {
    Parser parser;
    auto nr_consumed = parser.parse(msg);
    ASSERT_EQ(15, nr_consumed);
    const auto* command = command_as<SetCommand>(parser);
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(3U, command->body.size);
    const auto frame_size = command->body.frame_size();
    ASSERT_TRUE(frame_size.has_value());
    if (!frame_size) {
      return;
    }
    msg.remove_prefix(nr_consumed + *frame_size);
  }
  {
    Parser parser;
    auto nr_consumed = parser.parse(msg);
    ASSERT_EQ(9, nr_consumed);
    ASSERT_TRUE(has_command<GetCommand>(parser));
  }
}

TEST(ProtocolTest, parse_pipelined_headers_without_consuming_next_command) {
  using namespace sphinx::memcache;
  std::string_view msg = "get first\r\nget second\r\n";
  Parser first;
  auto first_consumed = first.parse(msg);
  ASSERT_EQ(first_consumed, 11U);
  const auto* first_command = command_as<GetCommand>(first);
  ASSERT_NE(first_command, nullptr);
  ASSERT_EQ(first_command->keys, (std::vector<std::string>{"first"}));

  msg.remove_prefix(first_consumed);
  Parser second;
  auto second_consumed = second.parse(msg);
  ASSERT_EQ(second_consumed, 12U);
  const auto* second_command = command_as<GetCommand>(second);
  ASSERT_NE(second_command, nullptr);
  ASSERT_EQ(second_command->keys, (std::vector<std::string>{"second"}));
}

TEST(ProtocolTest, parse_multi_get_preserves_order_and_owns_keys) {
  using namespace sphinx::memcache;
  std::string msg = "get first second first\r\nget after\r\n";
  Parser parser;
  auto nr_consumed = parser.parse(msg);

  ASSERT_EQ(nr_consumed, 24U);
  const auto* command = command_as<GetCommand>(parser);
  ASSERT_NE(command, nullptr);
  ASSERT_EQ(command->keys, (std::vector<std::string>{"first", "second", "first"}));

  // 键结果由 Parser 持有，而不是接收缓冲区中的视图；解析后 reactor 可以移动或
  // 释放该缓冲区。
  msg.clear();
  ASSERT_EQ(command->keys[1], "second");
  ASSERT_EQ(command->keys[2], "first");
}

TEST(ProtocolTest, parse_delete) {
  using namespace sphinx::memcache;
  Parser parser;
  auto msg = std::string{"delete gone\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  const auto* command = command_as<DeleteCommand>(parser);
  ASSERT_NE(command, nullptr);
  ASSERT_EQ(command->key, "gone");
}

TEST(ProtocolTest, parse_incr_and_decr_delta) {
  using namespace sphinx::memcache;
  {
    Parser parser;
    auto msg = std::string{"incr counter 0\r\n"};
    ASSERT_EQ(parser.parse(msg), msg.size());
    const auto* command = command_as<IncrCommand>(parser);
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->key, "counter");
    ASSERT_EQ(command->delta, 0U);
  }
  {
    Parser parser;
    auto msg = std::string{"decr counter 18446744073709551615\r\n"};
    ASSERT_EQ(parser.parse(msg), msg.size());
    const auto* command = command_as<DecrCommand>(parser);
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->delta, std::numeric_limits<uint64_t>::max());
    ASSERT_FALSE(parser.number_overflow());
  }
}

TEST(ProtocolTest, parse_delta_overflow_is_reported_without_losing_opcode) {
  using namespace sphinx::memcache;
  Parser parser;
  auto msg = std::string{"incr counter 18446744073709551616\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  ASSERT_TRUE(parser.number_overflow());
  ASSERT_TRUE(parser.command().has_value());
  const auto* command = command_as<IncrCommand>(parser);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->key, "counter");
}

TEST(ProtocolTest, parse_status_distinguishes_incomplete_and_invalid_headers) {
  using namespace sphinx::memcache;
  Parser parser;
  EXPECT_EQ(parser.status(), ParseStatus::Incomplete);
  EXPECT_EQ(parser.parse("get key"), 0U);
  EXPECT_EQ(parser.status(), ParseStatus::Incomplete);
  EXPECT_FALSE(parser.command().has_value());

  EXPECT_EQ(parser.parse("no-such-command\r\n"), 17U);
  EXPECT_EQ(parser.status(), ParseStatus::Invalid);
  EXPECT_FALSE(parser.command().has_value());
}

TEST(ProtocolTest, storage_body_view_preserves_incomplete_and_bad_terminator_boundaries) {
  using namespace sphinx::memcache;
  Parser parser;
  const auto header = std::string{"set foo 0 0 3\r\n"};
  ASSERT_EQ(parser.parse(header), header.size());
  const auto* command = command_as<SetCommand>(parser);
  ASSERT_NE(command, nullptr);

  EXPECT_FALSE(command->body.available(header + "bar"));
  EXPECT_FALSE(command->body.view(header + "bar").has_value());
  const auto malformed = header + "bar\rX";
  EXPECT_TRUE(command->body.available(malformed));
  EXPECT_FALSE(command->body.has_valid_terminator(malformed));
  EXPECT_FALSE(command->body.view(malformed).has_value());
}

TEST(ProtocolTest, storage_body_size_overflow_is_safe) {
  using namespace sphinx::memcache;
  Parser parser;
  const auto msg = std::string{"set foo 0 0 18446744073709551615\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  const auto* command = command_as<SetCommand>(parser);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->body.frame_size().has_value());
  EXPECT_FALSE(command->body.available(msg));
}

TEST(ProtocolTest, incomplete_header_requests_more_data) {
  using namespace sphinx::memcache;
  for (const auto& msg :
       {std::string{"get first"}, std::string{"delete first\r"}, std::string{"incr first 1"}}) {
    Parser parser;
    ASSERT_EQ(parser.parse(msg), 0U);
    ASSERT_EQ(parser.status(), ParseStatus::Incomplete);
    ASSERT_FALSE(parser.command().has_value());
  }
}

TEST(ProtocolTest, parse_mixed_pipeline_one_header_at_a_time) {
  using namespace sphinx::memcache;
  std::string_view msg = "get first second\r\ndelete old\r\nincr count 7\r\ndecr count 2\r\n";

  Parser get;
  auto get_consumed = get.parse(msg);
  ASSERT_EQ(get_consumed, 18U);
  const auto* get_command = command_as<GetCommand>(get);
  ASSERT_NE(get_command, nullptr);
  ASSERT_EQ(get_command->keys, (std::vector<std::string>{"first", "second"}));
  msg.remove_prefix(get_consumed);

  Parser remove;
  auto remove_consumed = remove.parse(msg);
  ASSERT_EQ(remove_consumed, 12U);
  const auto* remove_command = command_as<DeleteCommand>(remove);
  ASSERT_NE(remove_command, nullptr);
  ASSERT_EQ(remove_command->key, "old");
  msg.remove_prefix(remove_consumed);

  Parser incr;
  auto incr_consumed = incr.parse(msg);
  ASSERT_EQ(incr_consumed, 14U);
  const auto* incr_command = command_as<IncrCommand>(incr);
  ASSERT_NE(incr_command, nullptr);
  ASSERT_EQ(incr_command->delta, 7U);
  msg.remove_prefix(incr_consumed);

  Parser decr;
  ASSERT_EQ(decr.parse(msg), msg.find("\r\n") + 2);
  const auto* decr_command = command_as<DecrCommand>(decr);
  ASSERT_NE(decr_command, nullptr);
  ASSERT_EQ(decr_command->delta, 2U);
}

TEST(ProtocolTest, invalid_complete_command_does_not_consume_next_command) {
  using namespace sphinx::memcache;
  std::string_view msg = "delete first second\r\nget valid\r\n";

  Parser invalid;
  auto invalid_consumed = invalid.parse(msg);
  ASSERT_EQ(invalid_consumed, 21U);
  ASSERT_EQ(invalid.status(), ParseStatus::Invalid);
  ASSERT_FALSE(invalid.command().has_value());
  msg.remove_prefix(invalid_consumed);

  Parser valid;
  ASSERT_EQ(valid.parse(msg), 11U);
  const auto* valid_command = command_as<GetCommand>(valid);
  ASSERT_NE(valid_command, nullptr);
  ASSERT_EQ(valid_command->keys, (std::vector<std::string>{"valid"}));
}

TEST(ProtocolTest, parse_stats) {
  using namespace sphinx::memcache;
  Parser parser;
  auto msg = std::string{"stats\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  ASSERT_TRUE(has_command<StatsCommand>(parser));
}

TEST(ProtocolTest, parse_stats_header_in_fragments) {
  using namespace sphinx::memcache;
  Parser parser;
  ASSERT_EQ(parser.parse("sta"), 0U);
  ASSERT_EQ(parser.status(), ParseStatus::Incomplete);
  ASSERT_FALSE(parser.command().has_value());
  ASSERT_EQ(parser.parse("stats\r"), 0U);
  ASSERT_EQ(parser.status(), ParseStatus::Incomplete);
  ASSERT_FALSE(parser.command().has_value());

  auto complete = std::string{"stats\r\n"};
  ASSERT_EQ(parser.parse(complete), complete.size());
  ASSERT_TRUE(has_command<StatsCommand>(parser));
}

TEST(ProtocolTest, parse_stats_in_pipeline) {
  using namespace sphinx::memcache;
  std::string_view msg = "version\r\nstats\r\nget key\r\n";

  Parser version;
  auto version_consumed = version.parse(msg);
  ASSERT_EQ(version_consumed, 9U);
  ASSERT_TRUE(has_command<VersionCommand>(version));
  msg.remove_prefix(version_consumed);

  Parser stats;
  auto stats_consumed = stats.parse(msg);
  ASSERT_EQ(stats_consumed, 7U);
  ASSERT_TRUE(has_command<StatsCommand>(stats));
  msg.remove_prefix(stats_consumed);

  Parser get;
  ASSERT_EQ(get.parse(msg), 9U);
  const auto* get_command = command_as<GetCommand>(get);
  ASSERT_NE(get_command, nullptr);
  ASSERT_EQ(get_command->keys, (std::vector<std::string>{"key"}));
}
