// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <mysql.h>
#include <sphinx/mysql_product_store.h>

#include <atomic>

namespace {
std::atomic<bool> inject_error_after_real_commit{false};
std::atomic<int> commit_calls{0};
}  // namespace

// The optional test target links with --wrap=mysql_commit. Execute the real COMMIT once, then
// fake a lost/error response to test the adapter's ambiguous-commit branch deterministically.
using CommitReturn = decltype(mysql_commit(static_cast<MYSQL*>(nullptr)));
extern "C" CommitReturn __real_mysql_commit(MYSQL* connection);
extern "C" CommitReturn __wrap_mysql_commit(MYSQL* connection) {
  commit_calls.fetch_add(1);
  const auto result = __real_mysql_commit(connection);
  if (result == 0 && inject_error_after_real_commit.exchange(false)) {
    return static_cast<CommitReturn>(1);
  }
  return result;
}

namespace {

TEST(MySqlProductStoreIntegrationTest, ReadsExistingAndMissingProducts) {
  // TODO(agent): Require SPHINX_TEST_MYSQL_* fixture variables or GTEST_SKIP. Create a unique
  // fixture row in a disposable test schema; verify exact 64-bit values, valid UTF-8, and nullopt
  // only for an absent id. Drop the fixture row in RAII teardown. No production DB credentials.
  GTEST_SKIP() << "requires disposable MySQL 8.4 fixture";
}

TEST(MySqlProductStoreIntegrationTest, SerializesConcurrentVersionUpdates) {
  // TODO(agent): Two threads each own MySqlThreadGuard and MySqlProductStore, both start from the
  // same expected_version. Use a barrier; exactly one returns Updated and one Conflict. Then
  // read from a third connection and check version advanced exactly once.
  GTEST_SKIP() << "requires disposable MySQL 8.4 fixture";
}

TEST(MySqlProductStoreIntegrationTest, RollsBackKnownFailureAndDoesNotRetryUnknownCommit) {
  // TODO(agent): With a disposable fixture, set inject_error_after_real_commit=true immediately
  // before update(). The linker wrapper above lets the real COMMIT succeed but returns failure to
  // the adapter. Assert CommitUnknown, commit_calls increased exactly once, and a new connection
  // reads the incremented version. Also force a known pre-COMMIT error and assert rollback.
  GTEST_SKIP() << "requires disposable MySQL 8.4 fixture";
}

}  // namespace
