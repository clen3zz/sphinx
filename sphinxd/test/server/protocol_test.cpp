// Copyright 2018 The Sphinxd Authors.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <sphinx/protocol.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using sphinx::DeleteCommand;
using sphinx::GetCommand;
using sphinx::Parser;
using sphinx::ParseStatus;
using sphinx::SetCommand;
using sphinx::StatsCommand;
using sphinx::VersionCommand;

template <typename Command>
const Command* command_as(const Parser& parser) {
  const auto& parsed = parser.command();
  if (!parsed) {
    return nullptr;
  }
  return std::get_if<Command>(&parsed.value());
}

template <typename Command>
bool has_command(const Parser& parser) {
  const auto& parsed = parser.command();
  if (!parsed) {
    return false;
  }
  return std::holds_alternative<Command>(parsed.value());
}

}  // namespace

TEST(ProtocolTest, parse_error) {
  std::string const msg = "foo";
  Parser parser;
  parser.parse(msg);
  ASSERT_EQ(parser.status(), ParseStatus::Incomplete);
  ASSERT_FALSE(parser.command().has_value());
}

TEST(ProtocolTest, parse_set) {
  std::string const msg = "set foo 0 0 3\r\nbar\r\n";
  Parser parser;
  parser.parse(msg);
  ASSERT_TRUE(parser.command().has_value());
  ASSERT_TRUE(has_command<SetCommand>(parser));
}

TEST(ProtocolTest, parsed_command_is_typed_and_describes_storage_body) {
  std::string const msg = "set foo 7 11 3\r\nbar\r\n";
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
  std::string const msg = "get foo\r\n";
  Parser parser;
  parser.parse(msg);
  ASSERT_TRUE(parser.command().has_value());
  ASSERT_TRUE(has_command<GetCommand>(parser));
}

TEST(ProtocolTest, parse_many) {
  std::string const raw_msg = "set foo 0 0 3\r\nbar\r\nget foo\r\n";
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
  std::string_view msg = "get first\r\nget second\r\n";
  Parser first;
  auto first_consumed = first.parse(msg);
  ASSERT_EQ(first_consumed, 11U);
  const auto* first_command = command_as<GetCommand>(first);
  ASSERT_NE(first_command, nullptr);
  ASSERT_EQ(first_command->keys, std::vector<std::string>{"first"});

  msg.remove_prefix(first_consumed);
  Parser second;
  auto second_consumed = second.parse(msg);
  ASSERT_EQ(second_consumed, 12U);
  const auto* second_command = command_as<GetCommand>(second);
  ASSERT_NE(second_command, nullptr);
  ASSERT_EQ(second_command->keys, std::vector<std::string>{"second"});
}

TEST(ProtocolTest, parse_multi_get_preserves_order_and_owns_keys) {
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
  Parser parser;
  auto msg = std::string{"delete gone\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  const auto* command = command_as<DeleteCommand>(parser);
  ASSERT_NE(command, nullptr);
  ASSERT_EQ(command->key, "gone");
}

TEST(ProtocolTest, removed_commands_are_invalid) {
  for (const auto command :
       {"add key 0 0 1\r\n", "replace key 0 0 1\r\n", "incr key 1\r\n", "decr key 1\r\n"}) {
    Parser parser;
    ASSERT_EQ(parser.parse(command), std::string_view(command).size());
    EXPECT_EQ(parser.status(), ParseStatus::Invalid);
    EXPECT_FALSE(parser.command().has_value());
  }
}

TEST(ProtocolTest, set_number_overflow_is_reported) {
  Parser parser;
  auto msg = std::string{"set key 0 0 18446744073709551616\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  ASSERT_TRUE(parser.number_overflow());
  ASSERT_FALSE(parser.command().has_value());
}

TEST(ProtocolTest, parse_status_distinguishes_incomplete_and_invalid_headers) {
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
  Parser parser;
  const auto msg = std::string{"set foo 0 0 18446744073709551615\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  const auto* command = command_as<SetCommand>(parser);
  ASSERT_NE(command, nullptr);
  EXPECT_FALSE(command->body.frame_size().has_value());
  EXPECT_FALSE(command->body.available(msg));
}

TEST(ProtocolTest, incomplete_header_requests_more_data) {
  for (const auto& msg :
       {std::string{"get first"}, std::string{"delete first\r"}, std::string{"set first 0 0 1"}}) {
    Parser parser;
    ASSERT_EQ(parser.parse(msg), 0U);
    ASSERT_EQ(parser.status(), ParseStatus::Incomplete);
    ASSERT_FALSE(parser.command().has_value());
  }
}

TEST(ProtocolTest, parse_mixed_pipeline_one_header_at_a_time) {
  std::string_view msg = "get first second\r\ndelete old\r\nset count 0 0 1\r\n7\r\n";

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

  Parser set;
  auto set_consumed = set.parse(msg);
  ASSERT_EQ(set_consumed, 17U);
  const auto* set_command = command_as<SetCommand>(set);
  ASSERT_NE(set_command, nullptr);
  ASSERT_EQ(set_command->key, "count");
  ASSERT_EQ(set_command->body.view(msg), std::optional<std::string_view>{"7"});
}

TEST(ProtocolTest, invalid_complete_command_does_not_consume_next_command) {
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
  ASSERT_EQ(valid_command->keys, std::vector<std::string>{"valid"});
}

TEST(ProtocolTest, parse_stats) {
  Parser parser;
  auto msg = std::string{"stats\r\n"};
  ASSERT_EQ(parser.parse(msg), msg.size());
  ASSERT_TRUE(has_command<StatsCommand>(parser));
}

TEST(ProtocolTest, parse_stats_header_in_fragments) {
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
  ASSERT_EQ(get_command->keys, std::vector<std::string>{"key"});
}
