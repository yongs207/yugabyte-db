// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include "yb/util/random_util.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper_test_base.h"

DECLARE_int64(retryable_rpc_single_call_timeout_ms);
DECLARE_int32(yb_client_admin_operation_timeout_sec);

namespace yb {
namespace pgwrapper {

class PgLibPqTest : public PgWrapperTestBase {
 protected:
  virtual void SetUp() override {
    YBMiniClusterTestBase::SetUp();

    ExternalMiniClusterOptions opts;
    opts.start_pgsql_proxy = true;

    // TODO Increase the rpc timeout (from 2500) to not time out for long master queries (i.e. for
    // Postgres system tables). Should be removed once the long lock issue is fixed.
    int rpc_timeout = NonTsanVsTsan(10000, 30000);
    string rpc_flag = "--retryable_rpc_single_call_timeout_ms=";
    opts.extra_tserver_flags.emplace_back(rpc_flag + std::to_string(rpc_timeout));

    // With 3 tservers we'll be creating 3 tables per table, which is enough.
    opts.extra_tserver_flags.emplace_back("--yb_num_shards_per_tserver=1");
    opts.extra_tserver_flags.emplace_back("--pg_transactions_enabled");

    // Collect old records very aggressively to catch bugs with old readpoints.
    opts.extra_tserver_flags.emplace_back("--timestamp_history_retention_interval_sec=0");

    opts.extra_master_flags.emplace_back("--hide_pg_catalog_table_creation_logs");

    FLAGS_retryable_rpc_single_call_timeout_ms = rpc_timeout; // needed by cluster-wide initdb

    if (IsTsan()) {
      // Increase timeout for admin ops to account for create database with copying during initdb
      FLAGS_yb_client_admin_operation_timeout_sec = 120;
    }

    // Test that we can start PostgreSQL servers on non-colliding ports within each tablet server.
    opts.num_tablet_servers = 3;

    cluster_.reset(new ExternalMiniCluster(opts));
    ASSERT_OK(cluster_->Start());

    pg_ts = cluster_->tablet_server(0);

    // TODO: fix cluster verification for PostgreSQL tables.
    DontVerifyClusterBeforeNextTearDown();
  }

  Result<PGConnPtr> Connect() {
    PGConnPtr result(PQconnectdb(Format(
        "host=$0 port=$1 user=postgres", pg_ts->bind_host(), pg_ts->pgsql_rpc_port()).c_str()));
    auto status = PQstatus(result.get());
    if (status != ConnStatusType::CONNECTION_OK) {
      return STATUS_FORMAT(NetworkError, "Connect failed: $0", status);
    }
    return result;
  }

 protected:
  // Tablet server to use to perform PostgreSQL operations.
  ExternalTabletServer* pg_ts = nullptr;

};

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(Simple)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT, value TEXT)"));
  ASSERT_OK(Execute(conn.get(), "INSERT INTO t (key, value) VALUES (1, 'hello')"));

  auto res = ASSERT_RESULT(Fetch(conn.get(), "SELECT * FROM t"));

  {
    auto lines = PQntuples(res.get());
    ASSERT_EQ(1, lines);

    auto columns = PQnfields(res.get());
    ASSERT_EQ(2, columns);

    auto key = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(key, 1);
    auto value = ASSERT_RESULT(GetString(res.get(), 0, 1));
    ASSERT_EQ(value, "hello");
  }
}

// Test that repeats example from this article:
// https://blogs.msdn.microsoft.com/craigfr/2007/05/16/serializable-vs-snapshot-isolation-level/
//
// Multiple rows with values 0 and 1 are stored in table.
// Two concurrent transaction fetches all rows from table and does the following.
// First transaction changes value of all rows with value 0 to 1.
// Second transaction changes value of all rows with value 1 to 0.
// As outcome we should have rows with the same value.
//
// The described prodecure is repeated multiple times to increase probability of catching bug,
// w/o running test multiple times.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SerializableColoring)) {
  constexpr auto kKeys = RegularBuildVsSanitizers(10, 20);
  constexpr auto kColors = 2;
  constexpr auto kIterations = 20;

  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT PRIMARY KEY, color INT)"));

  auto iterations_left = kIterations;

  for (int iteration = 0; iterations_left > 0; ++iteration) {
    SCOPED_TRACE(Format("Iteration: $0", iteration));

    ASSERT_OK(Execute(conn.get(), "DELETE FROM t"));
    for (int k = 0; k != kKeys; ++k) {
      int32_t color = RandomUniformInt(0, kColors - 1);
      ASSERT_OK(Execute(conn.get(),
          Format("INSERT INTO t (key, color) VALUES ($0, $1)", k, color)));
    }

    std::atomic<int> complete{ 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i != kColors; ++i) {
      int32_t color = i;
      threads.emplace_back([this, color, kKeys, &complete] {
        auto conn = ASSERT_RESULT(Connect());

        ASSERT_OK(Execute(conn.get(), "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
        ASSERT_OK(Execute(conn.get(), "BEGIN"));

        auto res = Fetch(conn.get(), "SELECT * FROM t");
        if (!res.ok()) {
          auto msg = res.status().message().ToBuffer();
          ASSERT_TRUE(msg.find("Try again.") != std::string::npos) << res.status();
          return;
        }
        auto columns = PQnfields(res->get());
        ASSERT_EQ(2, columns);

        auto lines = PQntuples(res->get());
        ASSERT_EQ(kKeys, lines);
        for (int i = 0; i != lines; ++i) {
          if (ASSERT_RESULT(GetInt32(res->get(), i, 1)) == color) {
            continue;
          }

          auto key = ASSERT_RESULT(GetInt32(res->get(), i, 0));
          auto status = Execute(
              conn.get(), Format("UPDATE t SET color = $1 WHERE key = $0", key, color));
          if (!status.ok()) {
            auto msg = status.message().ToBuffer();
            // Missing metadata means that transaction was aborted and cleaned.
            ASSERT_TRUE(msg.find("Try again.") != std::string::npos ||
                        msg.find("Missing metadata") != std::string::npos) << status;
            break;
          }
        }

        auto status = Execute(conn.get(), "COMMIT");
        if (!status.ok()) {
          auto msg = status.message().ToBuffer();
          ASSERT_TRUE(msg.find("Operation expired") != std::string::npos) << status;
          return;
        }

        ++complete;
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    if (complete == 0) {
      continue;
    }

    auto res = ASSERT_RESULT(Fetch(conn.get(), "SELECT * FROM t"));
    auto columns = PQnfields(res.get());
    ASSERT_EQ(2, columns);

    auto lines = PQntuples(res.get());
    ASSERT_EQ(kKeys, lines);

    std::vector<int32_t> zeroes, ones;
    for (int i = 0; i != lines; ++i) {
      auto key = ASSERT_RESULT(GetInt32(res.get(), i, 0));
      auto current = ASSERT_RESULT(GetInt32(res.get(), i, 1));
      if (current == 0) {
        zeroes.push_back(key);
      } else {
        ones.push_back(key);
      }
    }

    std::sort(ones.begin(), ones.end());
    std::sort(zeroes.begin(), zeroes.end());

    LOG(INFO) << "Zeroes: " << yb::ToString(zeroes) << ", ones: " << yb::ToString(ones);
    ASSERT_TRUE(zeroes.empty() || ones.empty());

    --iterations_left;
  }
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SerializableReadWriteConflict)) {
  const auto kKeys = RegularBuildVsSanitizers(20, 5);

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT PRIMARY KEY)"));

  size_t reads_won = 0, writes_won = 0;
  for (int i = 0; i != kKeys; ++i) {
    auto read_conn = ASSERT_RESULT(Connect());
    ASSERT_OK(Execute(read_conn.get(), "BEGIN ISOLATION LEVEL REPEATABLE READ"));
    auto res = Fetch(read_conn.get(), Format("SELECT * FROM t WHERE key = $0", i));
    auto read_status = ResultToStatus(res);

    auto write_conn = ASSERT_RESULT(Connect());
    ASSERT_OK(Execute(write_conn.get(), "BEGIN ISOLATION LEVEL REPEATABLE READ"));
    auto write_status = Execute(write_conn.get(), Format("INSERT INTO t (key) VALUES ($0)", i));

    std::thread read_commit_thread([&read_conn, &read_status] {
      if (read_status.ok()) {
        read_status = Execute(read_conn.get(), "COMMIT");
      }
    });

    std::thread write_commit_thread([&write_conn, &write_status] {
      if (write_status.ok()) {
        write_status = Execute(write_conn.get(), "COMMIT");
      }
    });

    read_commit_thread.join();
    write_commit_thread.join();

    LOG(INFO) << "Read: " << read_status << ", write: " << write_status;

    if (!read_status.ok()) {
      ASSERT_OK(write_status);
      ++writes_won;
    } else {
      ASSERT_NOK(write_status);
      ++reads_won;
    }
  }

  LOG(INFO) << "Reads won: " << reads_won << ", writes won: " << writes_won;
  if (RegularBuildVsSanitizers(true, false)) {
    ASSERT_GE(reads_won, kKeys / 4);
    ASSERT_GE(writes_won, kKeys / 4);
  }
}

} // namespace pgwrapper
} // namespace yb
