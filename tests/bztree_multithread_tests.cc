// Copyright (c) Simon Fraser University
//
// Authors:
// Tianzheng Wang <tzwang@sfu.ca>
// Xiangpeng Hao <xiangpeng_hao@sfu.ca>

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <gperftools/profiler.h>
#include <random>

#include "util/performance_test.h"
#include "../bztree.h"

uint32_t descriptor_pool_size = 50000;

struct MultiThreadRead : public pmwcas::PerformanceTest {
  bztree::BzTree *tree;
  uint32_t read_count;

  explicit MultiThreadRead(uint32_t read_count, bztree::BzTree *tree)
      : PerformanceTest() {
    this->read_count = read_count;
    this->tree = tree;
    InsertDummy();
  }

  void InsertDummy() {
    for (uint64_t i = 0; i < read_count; i += 1) {
      std::string key = std::to_string(i);
      tree->Insert(key.c_str(), static_cast<uint16_t>(key.length()), i);
    }
  }

  void Entry(size_t thread_index) override {
    WaitForStart();
    uint64_t payload;
    for (uint32_t i = 0; i < read_count; i++) {
      auto key = std::to_string(i);
      ASSERT_TRUE(tree->Read(key.c_str(), key.length(), &payload).IsOk());
      ASSERT_EQ(payload, i);
    }
  }
};

struct MultiThreadInsertTest : public pmwcas::PerformanceTest {
  bztree::BzTree *tree;
  uint32_t item_per_thread;
  uint32_t thread_count;
  explicit MultiThreadInsertTest(uint32_t item_per_thread, uint32_t thread_count, bztree::BzTree *tree)
      : tree(tree), thread_count(thread_count), item_per_thread(item_per_thread) {}

  void SanityCheck() {
    std::vector<std::pair<uint32_t, bztree::InternalNode *>> value_missing;
    for (uint32_t i = 0; i < (thread_count + 1) * item_per_thread; i++) {
      auto i_str = std::to_string(i);
      uint64_t payload;
      auto rc = tree->Read(i_str.c_str(), static_cast<uint16_t>(i_str.length()), &payload);
      if (!rc.IsOk()) {
        bztree::Stack stack;
        auto leaf_node = tree->TraverseToLeaf(&stack, i_str.c_str(), i_str.length());
        LOG(INFO) << "sanity check failed at i = " << i << std::endl
                  << "rc: " << std::to_string(rc.rc) << std::endl
                  << "tree height: " << stack.num_frames + 1 << std::endl;
        auto parent_node = stack.Top();
        value_missing.emplace_back(i, parent_node->node);
      } else {
        ASSERT_TRUE(rc.IsOk());
        ASSERT_EQ(payload, i);
      }
    }
    for (const auto &pair:value_missing) {
      std::cout << "Value missing i = " << pair.first << std::endl
                << "=================" << std::endl;
      pair.second->Dump(tree->GetPMWCASPool()->GetEpoch(), true);
    }
    std::cout << std::endl;
    if (value_missing.size() > 0) {
      ASSERT_TRUE(false);
    }
  }

  void Entry(size_t thread_index) override {
    WaitForStart();
    for (uint32_t i = 0; i < item_per_thread * 2; i++) {
      auto value = i + item_per_thread * thread_index;
      auto str_value = std::to_string(value);
      auto rc = tree->Insert(str_value.c_str(),
                             static_cast<uint16_t>(str_value.length()), value);
      ASSERT_TRUE(rc.IsOk() || rc.IsKeyExists());
    }
  }
};

struct MultiThreadUpsertTest : public pmwcas::PerformanceTest {
  bztree::BzTree *tree;
  uint32_t item_per_thread;
  uint32_t thread_count;
  MultiThreadUpsertTest(uint32_t item_per_thread, uint32_t thread_count, bztree::BzTree *tree)
      : tree(tree), thread_count(thread_count), item_per_thread(item_per_thread) {}

  void SanityCheck() {
    for (uint32_t i = 0; i < (thread_count + 1) * item_per_thread * 10; i += 10) {
      auto i_str = std::to_string(i);
      uint64_t payload;
      auto rc = tree->Read(i_str.c_str(), static_cast<uint16_t>(i_str.length()), &payload);
      ASSERT_TRUE(rc.IsOk());
      ASSERT_TRUE(payload == i || payload == (i + 1));
    }
  }

  void Entry(size_t thread_index) override {
    std::vector<uint32_t> item_to_insert(item_per_thread * 2);
    for (uint32_t i = 0; i < item_per_thread * 2; i += 1) {
      item_to_insert.emplace_back(i + thread_index * item_per_thread);
    }
    static std::random_device rd;
    static std::mt19937 g(rd());
    std::shuffle(item_to_insert.begin(), item_to_insert.end(), g);

    WaitForStart();
    // for odd index insert value+1
    // for even index insert value
    // since half of the keys are duplicate
    // this will force upsert to trigger both "insert" and "update"
    for (uint32_t i = 0; i < item_per_thread * 2; i += 1) {
      auto value = 10 * (i + item_per_thread * thread_index);
      auto str_value = std::to_string(value);
      auto rc = tree->Upsert(str_value.c_str(),
                             static_cast<uint16_t>(str_value.length()),
                             i % 2 == 0 ? value : value + 1);
    }
  }
};

GTEST_TEST(BztreeTest, MultiThreadRead) {
//  auto thread_count = pmwcas::Environment::Get()->GetCoreCount();
  uint32_t thread_count = 8;
  std::unique_ptr<pmwcas::DescriptorPool> pool(
      new pmwcas::DescriptorPool(descriptor_pool_size, 5, false)
  );
  bztree::BzTree::ParameterSet param;
  std::unique_ptr<bztree::BzTree> tree(bztree::BzTree::New(param, pool.get()));
  MultiThreadRead t(10000, tree.get());
  t.Run(thread_count);
  pmwcas::Thread::ClearRegistry(true);
}

GTEST_TEST(BztreeTest, MultiThreadInsertTest) {
  uint32_t thread_count = 50;
  uint32_t item_per_thread = 100;
  std::unique_ptr<pmwcas::DescriptorPool> pool(
      new pmwcas::DescriptorPool(descriptor_pool_size, thread_count, false)
  );
  const auto kb = 1024;
  bztree::BzTree::ParameterSet param(kb * kb, 0, kb * kb);
  std::unique_ptr<bztree::BzTree> tree(bztree::BzTree::New(param, pool.get()));

  MultiThreadInsertTest t(item_per_thread, thread_count, tree.get());
  t.Run(thread_count);
  t.SanityCheck();
  pmwcas::Thread::ClearRegistry(true);
}
GTEST_TEST(BztreeTest, MultiThreadInsertSplitTest) {
  uint32_t thread_count = 10;
  uint32_t item_per_thread = 300;
  std::unique_ptr<pmwcas::DescriptorPool> pool(
      new pmwcas::DescriptorPool(50000, thread_count, false)
  );
  bztree::BzTree::ParameterSet param;
  std::unique_ptr<bztree::BzTree> tree(bztree::BzTree::New(param, pool.get()));
  MultiThreadInsertTest t(item_per_thread, thread_count, tree.get());
  t.Run(thread_count);
  t.SanityCheck();
  pmwcas::Thread::ClearRegistry(true);
}
GTEST_TEST(BztreeTest, MultiThreadInsertInternalSplitTest) {
  uint32_t thread_count = 50;
  uint32_t item_per_thread = 10000;
  std::unique_ptr<pmwcas::DescriptorPool> pool(
      new pmwcas::DescriptorPool(descriptor_pool_size, thread_count, false)
  );
  bztree::BzTree::ParameterSet param(256, 0, 256);
  std::unique_ptr<bztree::BzTree> tree(bztree::BzTree::New(param, pool.get()));
  MultiThreadInsertTest t(item_per_thread, thread_count, tree.get());
  t.Run(thread_count);
  t.SanityCheck();
  pmwcas::Thread::ClearRegistry(true);
}

GTEST_TEST(BztreeTest, MiltiUpsertTest) {
  uint32_t thread_count = 50;
  uint32_t item_per_thread = 1000;
  std::unique_ptr<pmwcas::DescriptorPool> pool(
      new pmwcas::DescriptorPool(descriptor_pool_size, thread_count, false)
  );
  bztree::BzTree::ParameterSet param(256, 0, 256);
  std::unique_ptr<bztree::BzTree> tree(bztree::BzTree::New(param, pool.get()));
  MultiThreadUpsertTest t(item_per_thread, thread_count, tree.get());
  t.Run(thread_count);
  t.SanityCheck();
  pmwcas::Thread::ClearRegistry(true);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  pmwcas::InitLibrary(pmwcas::DefaultAllocator::Create,
                      pmwcas::DefaultAllocator::Destroy,
                      pmwcas::LinuxEnvironment::Create,
                      pmwcas::LinuxEnvironment::Destroy);
  return RUN_ALL_TESTS();
}
