// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/util/maintenance_manager.h"

#include <math.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>

#include <boost/bind.hpp> // IWYU pragma: keep
#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/maintenance_manager.pb.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"
#include "kudu/util/mutex.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"
#include "kudu/util/thread.h"

using std::list;
using std::shared_ptr;
using std::string;
using strings::Substitute;

METRIC_DEFINE_entity(test);
METRIC_DEFINE_gauge_uint32(test, maintenance_ops_running,
                           "Number of Maintenance Operations Running",
                           kudu::MetricUnit::kMaintenanceOperations,
                           "The number of background maintenance operations currently running.");
METRIC_DEFINE_histogram(test, maintenance_op_duration,
                        "Maintenance Operation Duration",
                        kudu::MetricUnit::kSeconds, "", 60000000LU, 2);

DECLARE_int64(log_target_replay_size_mb);
DECLARE_string(maintenance_manager_table_priorities);
DECLARE_double(maintenance_op_multiplier);
DECLARE_int32(max_priority_range);


namespace kudu {

static const int kHistorySize = 6;
static const char kFakeUuid[] = "12345";

class MaintenanceManagerTest : public KuduTest {
 public:
  void SetUp() override {
    StartManager(2);
  }

  void TearDown() override {
    StopManager();
  }

  void StartManager(int32_t num_threads) {
    MaintenanceManager::Options options;
    options.num_threads = num_threads;
    options.polling_interval_ms = 1;
    options.history_size = kHistorySize;
    manager_.reset(new MaintenanceManager(options, kFakeUuid));
    manager_->set_memory_pressure_func_for_tests(
        [&](double* consumption) {
          return indicate_memory_pressure_.load();
        });
    ASSERT_OK(manager_->Start());
  }

  void StopManager() {
    manager_->Shutdown();
  }

 protected:
  shared_ptr<MaintenanceManager> manager_;
  std::atomic<bool> indicate_memory_pressure_ { false };
};

// Just create the MaintenanceManager and then shut it down, to make sure
// there are no race conditions there.
TEST_F(MaintenanceManagerTest, TestCreateAndShutdown) {
}

class TestMaintenanceOp : public MaintenanceOp {
 public:
  TestMaintenanceOp(const std::string& name,
                    IOUsage io_usage,
                    std::string table_id = "fake.table_id")
    : MaintenanceOp(name, io_usage),
      ram_anchored_(500),
      logs_retained_bytes_(0),
      perf_improvement_(0),
      metric_entity_(METRIC_ENTITY_test.Instantiate(&metric_registry_, "test")),
      maintenance_op_duration_(METRIC_maintenance_op_duration.Instantiate(metric_entity_)),
      maintenance_ops_running_(METRIC_maintenance_ops_running.Instantiate(metric_entity_, 0)),
      remaining_runs_(1),
      prepared_runs_(0),
      sleep_time_(MonoDelta::FromSeconds(0)),
      table_id_(std::move(table_id)) {
  }

  ~TestMaintenanceOp() override = default;

  bool Prepare() override {
    std::lock_guard<Mutex> guard(lock_);
    if (remaining_runs_ == 0) {
      return false;
    }
    remaining_runs_--;
    prepared_runs_++;
    DLOG(INFO) << "Prepared op " << name();
    return true;
  }

  void Perform() override {
    {
      std::lock_guard<Mutex> guard(lock_);
      DLOG(INFO) << "Performing op " << name();

      // Ensure that we don't call Perform() more times than we returned
      // success from Prepare().
      CHECK_GE(prepared_runs_, 1);
      prepared_runs_--;
    }

    SleepFor(sleep_time_);
  }

  void UpdateStats(MaintenanceOpStats* stats) override {
    std::lock_guard<Mutex> guard(lock_);
    stats->set_runnable(remaining_runs_ > 0);
    stats->set_ram_anchored(ram_anchored_);
    stats->set_logs_retained_bytes(logs_retained_bytes_);
    stats->set_perf_improvement(perf_improvement_);
  }

  void set_remaining_runs(int runs) {
    std::lock_guard<Mutex> guard(lock_);
    remaining_runs_ = runs;
  }

  void set_sleep_time(MonoDelta time) {
    std::lock_guard<Mutex> guard(lock_);
    sleep_time_ = time;
  }

  void set_ram_anchored(uint64_t ram_anchored) {
    std::lock_guard<Mutex> guard(lock_);
    ram_anchored_ = ram_anchored;
  }

  void set_logs_retained_bytes(uint64_t logs_retained_bytes) {
    std::lock_guard<Mutex> guard(lock_);
    logs_retained_bytes_ = logs_retained_bytes;
  }

  void set_perf_improvement(uint64_t perf_improvement) {
    std::lock_guard<Mutex> guard(lock_);
    perf_improvement_ = perf_improvement;
  }

  scoped_refptr<Histogram> DurationHistogram() const override {
    return maintenance_op_duration_;
  }

  scoped_refptr<AtomicGauge<uint32_t> > RunningGauge() const override {
    return maintenance_ops_running_;
  }

  const std::string& table_id() const override {
    return table_id_;
  }

 private:
  Mutex lock_;

  uint64_t ram_anchored_;
  uint64_t logs_retained_bytes_;
  uint64_t perf_improvement_;
  MetricRegistry metric_registry_;
  scoped_refptr<MetricEntity> metric_entity_;
  scoped_refptr<Histogram> maintenance_op_duration_;
  scoped_refptr<AtomicGauge<uint32_t> > maintenance_ops_running_;

  // The number of remaining times this operation will run before disabling
  // itself.
  int remaining_runs_;
  // The number of Prepared() operations which have not yet been Perform()ed.
  int prepared_runs_;

  // The amount of time each op invocation will sleep.
  MonoDelta sleep_time_;
  std::string table_id_;
};

// Create an op and wait for it to start running.  Unregister it while it is
// running and verify that UnregisterOp waits for it to finish before
// proceeding.
TEST_F(MaintenanceManagerTest, TestRegisterUnregister) {
  TestMaintenanceOp op1("1", MaintenanceOp::HIGH_IO_USAGE);
  op1.set_perf_improvement(10);
  // Register initially with no remaining runs. We'll later enable it once it's
  // already registered.
  op1.set_remaining_runs(0);
  manager_->RegisterOp(&op1);
  scoped_refptr<kudu::Thread> thread;
  CHECK_OK(Thread::Create(
      "TestThread", "TestRegisterUnregister",
      boost::bind(&TestMaintenanceOp::set_remaining_runs, &op1, 1), &thread));
  ASSERT_EVENTUALLY([&]() {
      ASSERT_EQ(op1.DurationHistogram()->TotalCount(), 1);
    });
  manager_->UnregisterOp(&op1);
  ThreadJoiner(thread.get()).Join();
}

// Regression test for KUDU-1495: when an operation is being unregistered,
// new instances of that operation should not be scheduled.
TEST_F(MaintenanceManagerTest, TestNewOpsDontGetScheduledDuringUnregister) {
  TestMaintenanceOp op1("1", MaintenanceOp::HIGH_IO_USAGE);
  op1.set_perf_improvement(10);

  // Set the op to run up to 10 times, and each time should sleep for a second.
  op1.set_remaining_runs(10);
  op1.set_sleep_time(MonoDelta::FromSeconds(1));
  manager_->RegisterOp(&op1);

  // Wait until two instances of the ops start running, since we have two MM threads.
  ASSERT_EVENTUALLY([&]() {
      ASSERT_EQ(op1.RunningGauge()->value(), 2);
    });

  // Trigger Unregister while they are running. This should wait for the currently-
  // running operations to complete, but no new operations should be scheduled.
  manager_->UnregisterOp(&op1);

  // Hence, we should have run only the original two that we saw above.
  ASSERT_LE(op1.DurationHistogram()->TotalCount(), 2);
}

// Test that we'll run an operation that doesn't improve performance when memory
// pressure gets high.
TEST_F(MaintenanceManagerTest, TestMemoryPressure) {
  TestMaintenanceOp op("op", MaintenanceOp::HIGH_IO_USAGE);
  op.set_ram_anchored(100);
  manager_->RegisterOp(&op);

  // At first, we don't want to run this, since there is no perf_improvement.
  SleepFor(MonoDelta::FromMilliseconds(20));
  ASSERT_EQ(0, op.DurationHistogram()->TotalCount());

  // Fake that the server is under memory pressure.
  indicate_memory_pressure_ = true;

  ASSERT_EVENTUALLY([&]() {
      ASSERT_EQ(op.DurationHistogram()->TotalCount(), 1);
    });
  manager_->UnregisterOp(&op);
}

// Test that ops are prioritized correctly when we add log retention.
TEST_F(MaintenanceManagerTest, TestLogRetentionPrioritization) {
  const int64_t kMB = 1024 * 1024;

  StopManager();

  TestMaintenanceOp op1("op1", MaintenanceOp::LOW_IO_USAGE);
  op1.set_ram_anchored(0);
  op1.set_logs_retained_bytes(100 * kMB);

  TestMaintenanceOp op2("op2", MaintenanceOp::HIGH_IO_USAGE);
  op2.set_ram_anchored(100);
  op2.set_logs_retained_bytes(100 * kMB);

  TestMaintenanceOp op3("op3", MaintenanceOp::HIGH_IO_USAGE);
  op3.set_ram_anchored(200);
  op3.set_logs_retained_bytes(100 * kMB);

  manager_->RegisterOp(&op1);
  manager_->RegisterOp(&op2);
  manager_->RegisterOp(&op3);

  // We want to do the low IO op first since it clears up some log retention.
  auto op_and_why = manager_->FindBestOp();
  ASSERT_EQ(&op1, op_and_why.first);
  EXPECT_EQ(op_and_why.second, "free 104857600 bytes of WAL");

  manager_->UnregisterOp(&op1);

  // Low IO is taken care of, now we find the op that clears the most log retention and ram.
  // However, with the default settings, we won't bother running any of these operations
  // which only retain 100MB of logs.
  op_and_why = manager_->FindBestOp();
  ASSERT_EQ(nullptr, op_and_why.first);
  EXPECT_EQ(op_and_why.second, "no ops with positive improvement");

  // If we change the target WAL size, we will select these ops.
  FLAGS_log_target_replay_size_mb = 50;
  op_and_why = manager_->FindBestOp();
  ASSERT_EQ(&op3, op_and_why.first);
  EXPECT_EQ(op_and_why.second, "104857600 bytes log retention, and flush 200 bytes memory");

  manager_->UnregisterOp(&op3);

  op_and_why = manager_->FindBestOp();
  ASSERT_EQ(&op2, op_and_why.first);
  EXPECT_EQ(op_and_why.second, "104857600 bytes log retention, and flush 100 bytes memory");

  manager_->UnregisterOp(&op2);
}

// Test retrieving a list of an op's running instances
TEST_F(MaintenanceManagerTest, TestRunningInstances) {
  TestMaintenanceOp op("op", MaintenanceOp::HIGH_IO_USAGE);
  op.set_perf_improvement(10);
  op.set_remaining_runs(2);
  op.set_sleep_time(MonoDelta::FromSeconds(1));
  manager_->RegisterOp(&op);

  // Check that running instances are added to the maintenance manager's collection,
  // and fields are getting filled.
  ASSERT_EVENTUALLY([&]() {
      MaintenanceManagerStatusPB status_pb;
      manager_->GetMaintenanceManagerStatusDump(&status_pb);
      ASSERT_EQ(status_pb.running_operations_size(), 2);
      const MaintenanceManagerStatusPB_OpInstancePB& instance1 = status_pb.running_operations(0);
      const MaintenanceManagerStatusPB_OpInstancePB& instance2 = status_pb.running_operations(1);
      ASSERT_EQ(instance1.name(), op.name());
      ASSERT_NE(instance1.thread_id(), instance2.thread_id());
    });

  // Wait for instances to complete.
  manager_->UnregisterOp(&op);

  // Check that running instances are removed from collection after completion.
  MaintenanceManagerStatusPB status_pb;
  manager_->GetMaintenanceManagerStatusDump(&status_pb);
  ASSERT_EQ(status_pb.running_operations_size(), 0);
}

// Test adding operations and make sure that the history of recently completed operations
// is correct in that it wraps around and doesn't grow.
TEST_F(MaintenanceManagerTest, TestCompletedOpsHistory) {
  for (int i = 0; i < kHistorySize + 1; i++) {
    string name = Substitute("op$0", i);
    TestMaintenanceOp op(name, MaintenanceOp::HIGH_IO_USAGE);
    op.set_perf_improvement(1);
    op.set_ram_anchored(100);
    manager_->RegisterOp(&op);

    ASSERT_EVENTUALLY([&]() {
        ASSERT_EQ(op.DurationHistogram()->TotalCount(), 1);
      });
    manager_->UnregisterOp(&op);

    MaintenanceManagerStatusPB status_pb;
    manager_->GetMaintenanceManagerStatusDump(&status_pb);
    // The size should equal to the current completed OP size,
    // and should be at most the kHistorySize.
    ASSERT_EQ(std::min(kHistorySize, i + 1), status_pb.completed_operations_size());
    // The most recently completed op should always be first, even if we wrap
    // around.
    ASSERT_EQ(name, status_pb.completed_operations(0).name());
  }
}

// Test maintenance OP factors.
// The OPs on different priority levels have different OP score multipliers.
TEST_F(MaintenanceManagerTest, TestOpFactors) {
  StopManager();

  ASSERT_GE(FLAGS_max_priority_range, 1);
  ASSERT_NE("", gflags::SetCommandLineOption(
      "maintenance_manager_table_priorities",
      Substitute("table_id_1:$0;table_id_2:$1;table_id_3:$2;table_id_4:$3;table_id_5:$4",
                 -FLAGS_max_priority_range - 1, -1, 0, 1, FLAGS_max_priority_range + 1).c_str()));
  TestMaintenanceOp op1("op1", MaintenanceOp::HIGH_IO_USAGE, "table_id_1");
  TestMaintenanceOp op2("op2", MaintenanceOp::HIGH_IO_USAGE, "table_id_2");
  TestMaintenanceOp op3("op3", MaintenanceOp::HIGH_IO_USAGE, "table_id_3");
  TestMaintenanceOp op4("op4", MaintenanceOp::HIGH_IO_USAGE, "table_id_4");
  TestMaintenanceOp op5("op5", MaintenanceOp::HIGH_IO_USAGE, "table_id_5");
  TestMaintenanceOp op6("op6", MaintenanceOp::HIGH_IO_USAGE, "table_id_6");

  manager_->UpdateTablePriorities();

  ASSERT_DOUBLE_EQ(pow(FLAGS_maintenance_op_multiplier, -FLAGS_max_priority_range),
                   manager_->PerfImprovement(1, op1.table_id()));
  ASSERT_DOUBLE_EQ(pow(FLAGS_maintenance_op_multiplier, -1),
                   manager_->PerfImprovement(1, op2.table_id()));
  ASSERT_DOUBLE_EQ(1, manager_->PerfImprovement(1, op3.table_id()));
  ASSERT_DOUBLE_EQ(FLAGS_maintenance_op_multiplier, manager_->PerfImprovement(1, op4.table_id()));
  ASSERT_DOUBLE_EQ(pow(FLAGS_maintenance_op_multiplier, FLAGS_max_priority_range),
                   manager_->PerfImprovement(1, op5.table_id()));
  ASSERT_DOUBLE_EQ(1, manager_->PerfImprovement(1, op6.table_id()));
}

// Test priority OP launching.
TEST_F(MaintenanceManagerTest, TestPriorityOpLaunch) {
  StopManager();
  StartManager(1);

  ASSERT_NE("", gflags::SetCommandLineOption(
      "maintenance_manager_table_priorities",
      Substitute("table_id_1:$0;table_id_2:$1;table_id_3:$2;table_id_4:$3;table_id_5:$4",
                 -FLAGS_max_priority_range - 1, -1, 0, 1, FLAGS_max_priority_range + 1).c_str()));

  TestMaintenanceOp op1("op1", MaintenanceOp::HIGH_IO_USAGE, "table_id_1");
  op1.set_perf_improvement(10);
  op1.set_remaining_runs(1);
  op1.set_sleep_time(MonoDelta::FromMilliseconds(1));

  TestMaintenanceOp op2("op2", MaintenanceOp::HIGH_IO_USAGE, "table_id_2");
  op2.set_perf_improvement(10);
  op2.set_remaining_runs(1);
  op2.set_sleep_time(MonoDelta::FromMilliseconds(1));

  TestMaintenanceOp op3("op3", MaintenanceOp::HIGH_IO_USAGE, "table_id_3");
  op3.set_perf_improvement(10);
  op3.set_remaining_runs(1);
  op3.set_sleep_time(MonoDelta::FromMilliseconds(1));

  TestMaintenanceOp op4("op4", MaintenanceOp::HIGH_IO_USAGE, "table_id_4");
  op4.set_perf_improvement(10);
  op4.set_remaining_runs(1);
  op4.set_sleep_time(MonoDelta::FromMilliseconds(1));

  TestMaintenanceOp op5("op5", MaintenanceOp::HIGH_IO_USAGE, "table_id_5");
  op5.set_perf_improvement(10);
  op5.set_remaining_runs(1);
  op5.set_sleep_time(MonoDelta::FromMilliseconds(1));

  TestMaintenanceOp op6("op6", MaintenanceOp::HIGH_IO_USAGE, "table_id_6");
  op6.set_perf_improvement(12);
  op6.set_remaining_runs(1);
  op6.set_sleep_time(MonoDelta::FromMilliseconds(1));

  manager_->RegisterOp(&op1);
  manager_->RegisterOp(&op2);
  manager_->RegisterOp(&op3);
  manager_->RegisterOp(&op4);
  manager_->RegisterOp(&op5);
  manager_->RegisterOp(&op6);

  ASSERT_EVENTUALLY([&]() {
    MaintenanceManagerStatusPB status_pb;
    manager_->GetMaintenanceManagerStatusDump(&status_pb);
    ASSERT_EQ(status_pb.completed_operations_size(), 6);
  });

  // Wait for instances to complete.
  manager_->UnregisterOp(&op1);
  manager_->UnregisterOp(&op2);
  manager_->UnregisterOp(&op3);
  manager_->UnregisterOp(&op4);
  manager_->UnregisterOp(&op5);
  manager_->UnregisterOp(&op6);

  // Check that running instances are removed from collection after completion.
  MaintenanceManagerStatusPB status_pb;
  manager_->GetMaintenanceManagerStatusDump(&status_pb);
  ASSERT_EQ(status_pb.running_operations_size(), 0);
  ASSERT_EQ(status_pb.completed_operations_size(), 6);
  // In perf_improvement score ascending order, the latter completed OP will list former.
  list<string> ordered_ops({"op1",
                            "op2",
                            "op3",
                            "op4",
                            "op6",
                            "op5"});
  ASSERT_EQ(ordered_ops.size(), status_pb.completed_operations().size());
  for (const auto& instance : status_pb.completed_operations()) {
    ASSERT_EQ(ordered_ops.front(), instance.name());
    ordered_ops.pop_front();
  }
}

} // namespace kudu
