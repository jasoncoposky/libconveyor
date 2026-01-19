#include "libconveyor/conveyor.h"
#include <gtest/gtest.h>


#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>


// --- Mock Storage Backend ---
class MockStorage {
public:
  std::vector<char> data;
  std::mutex mx;
  off_t current_pos = 0; // Simulate file pointer

  MockStorage(size_t size) : data(size, 0) {}

  static ssize_t pwrite_callback(storage_handle_t h, const void *buf,
                                 size_t count, off_t offset) {
    auto *self = static_cast<MockStorage *>(h);
    std::lock_guard<std::mutex> lock(self->mx);
    if (offset + count > self->data.size()) {
      self->data.resize(offset + count);
    }
    std::memcpy(self->data.data() + offset, buf, count);
    return count;
  }

  static ssize_t pread_callback(storage_handle_t h, void *buf, size_t count,
                                off_t offset) {
    auto *self = static_cast<MockStorage *>(h);
    std::lock_guard<std::mutex> lock(self->mx);
    if (offset >= (off_t)self->data.size())
      return 0;
    size_t available =
        std::min((size_t)count, self->data.size() - (size_t)offset);
    std::memcpy(buf, self->data.data() + offset, available);
    return available;
  }

  static off_t lseek_callback(storage_handle_t h, off_t offset, int whence) {
    auto *self = static_cast<MockStorage *>(h);
    std::lock_guard<std::mutex> lock(self->mx);
    if (whence == SEEK_SET)
      self->current_pos = offset;
    else if (whence == SEEK_CUR)
      self->current_pos += offset;
    else if (whence == SEEK_END)
      self->current_pos = static_cast<off_t>(self->data.size()) + offset;
    return self->current_pos;
  }

  storage_operations_t get_ops() {
    return {pwrite_callback, pread_callback, lseek_callback};
  }
};

// --- Test Fixture ---
class ConveyorConsistencyTest : public ::testing::Test {
protected:
  MockStorage *mock;
  conveyor_t *conv;

  void SetUp() override {
    mock = new MockStorage(1024);
    conv = nullptr;
  }

  void TearDown() override {
    if (conv)
      conveyor_destroy(conv);
    delete mock;
  }
};

// Verifies 'recovery-style' sequential reading:
// 1. Existing file with data
// 2. Open conveyor with offset at 0
// 3. Sequential reads until EOF
TEST_F(ConveyorConsistencyTest, SequentialReadRecovery) {
  auto ops = mock->get_ops();

  // 1. Setup existing data (simulating a WAL)
  std::string existing_data;
  for (int i = 0; i < 5000; i++)
    existing_data += "REC" + std::to_string(i) + "|";
  mock->data.resize(existing_data.size());
  std::memcpy(mock->data.data(), existing_data.c_str(), existing_data.size());

  // Ensure "file pointer" is at 0 initially (like opening a file)
  mock->current_pos = 0;

  conveyor_config_t cfg = {0};
  cfg.handle = mock;
  cfg.flags = O_RDONLY;
  cfg.ops = ops;
  cfg.initial_write_size = 0;   // Read-only
  cfg.initial_read_size = 4096; // 4KB read buffer
  cfg.max_read_size = 1024 * 1024;

  conv = conveyor_create(&cfg);
  ASSERT_NE(conv, nullptr);

  std::vector<char> read_back;
  read_back.reserve(existing_data.size());

  char buf[1024];
  while (true) {
    ssize_t res = conveyor_read(conv, buf, sizeof(buf));
    if (res < 0)
      FAIL() << "Read error: " << errno;
    if (res == 0)
      break; // EOF
    read_back.insert(read_back.end(), buf, buf + res);
  }

  ASSERT_EQ(read_back.size(), existing_data.size());
  std::string read_str(read_back.begin(), read_back.end());
  ASSERT_EQ(read_str, existing_data);
}
