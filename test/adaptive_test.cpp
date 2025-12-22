#include <gtest/gtest.h>
#include "mock_storage.hpp"
#include "libconveyor/conveyor.h"

class AdaptiveTest : public ::testing::Test {
protected:
    MockStorage* mock;
    
    void SetUp() override {
        mock = new MockStorage(1024 * 1024);
    }
    void TearDown() override {
        delete mock;
    }
};

// Test 1: Simple Growth
// Write more than initial capacity, verify no error.
TEST_F(AdaptiveTest, WriteTriggeredGrowth) {
    auto ops = mock->get_ops();
    conveyor_config_t cfg = {0};
    cfg.handle = (storage_handle_t)mock;
    cfg.ops = ops;
    cfg.flags = O_RDWR;
    
    // Start VERY small (100 bytes)
    cfg.initial_write_size = 100;
    cfg.max_write_size = 1000;
    cfg.initial_read_size = 0; // Ensure read buffer is disabled for write test
    cfg.max_read_size = 0;     // Ensure read buffer is disabled for write test
    
    conveyor_t* conv = conveyor_create(&cfg);
    
    // Write 150 bytes (Trigger Growth)
    std::vector<char> data(150, 'A');
    ssize_t res = conveyor_write(conv, data.data(), 150);
    
    ASSERT_EQ(res, 150);
    
    // Verify Data integrity
    conveyor_flush(conv);
    ASSERT_EQ(std::memcmp(mock->data.data(), data.data(), 150), 0);
    
    conveyor_destroy(conv);
}

// Test 2: The "Wrapped Resize" Torture Test
// 1. Fill buffer to end.
// 2. Consume half (advancing tail).
// 3. Write more (wrapping head to beginning).
// 4. Force resize (Must unroll properly).
TEST_F(AdaptiveTest, ResizeWhileWrapped) {
    auto ops = mock->get_ops();
    conveyor_config_t cfg = {0};
    cfg.handle = (storage_handle_t)mock;
    cfg.ops = ops;
    cfg.flags = O_RDWR;
    
    // Size 100. Max 500.
    cfg.initial_write_size = 100;
    cfg.max_write_size = 500;
    cfg.initial_read_size = 0; // Ensure read buffer is disabled for write test
    cfg.max_read_size = 0;     // Ensure read buffer is disabled for write test
    
    conveyor_t* conv = conveyor_create(&cfg);
    
    // 1. Pause the worker so the buffer fills up and stays filled
    // (We can't easily pause the worker from public API, but we can simulate
    // slow I/O using the mock to ensure queue builds up)
    mock->write_delay_ms = 500; 

    // 2. Write 80 bytes (Buffer: [0...80...100])
    // Tail=0, Head=80
    std::vector<char> chunk1(80, '1');
    conveyor_write(conv, chunk1.data(), 80);

    // 3. Let worker drain 50 bytes (Wait > 500ms)
    // Tail advances to 50. Head remains 80.
    // Buffer logic state: [Empty(0-50) | Data(50-80) | Empty(80-100)]
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    
    // 4. Write 40 bytes.
    // This fits! 20 bytes at end (80-100), 20 bytes at start (0-20).
    // Buffer is now WRAPPED. Tail=50, Head=20.
    std::vector<char> chunk2(40, '2');
    conveyor_write(conv, chunk2.data(), 40);
    
    // 5. NOW WRITE 200 Bytes.
    // This forces a Resize on a Wrapped Buffer.
    // The resize must detect Head < Tail and copy two segments correctly.
    std::vector<char> chunk3(200, '3');
    ssize_t res = conveyor_write(conv, chunk3.data(), 200);
    
    ASSERT_EQ(res, 200);

    // 6. Verify data on disk
    conveyor_flush(conv);
    
    // Expected Layout:
    // 0-80: '1'
    // 80-120: '2'
    // 120-320: '3'
    std::vector<char> expected;
    expected.insert(expected.end(), chunk1.begin(), chunk1.end());
    expected.insert(expected.end(), chunk2.begin(), chunk2.end());
    expected.insert(expected.end(), chunk3.begin(), chunk3.end());
    
    ASSERT_EQ(std::memcmp(mock->data.data(), expected.data(), expected.size()), 0);

    conveyor_destroy(conv);
}

// Test 3: Read Heuristic (Sequential Exhaustion)
TEST_F(AdaptiveTest, ReadSequentialGrowth) {
    auto ops = mock->get_ops();
    conveyor_config_t cfg = {0};
    cfg.handle = (storage_handle_t)mock;
    cfg.ops = ops;
    cfg.flags = O_RDONLY;
    cfg.initial_read_size = 128; // Small start
    cfg.max_read_size = 4096;    // Allow growth
    cfg.initial_write_size = 0;  // Ensure write buffer is disabled for read test
    cfg.max_write_size = 0;      // Ensure write buffer is disabled for read test
    
    conveyor_t* conv = conveyor_create(&cfg);
    
    // Populate mock with 2KB of data
    std::fill(mock->data.begin(), mock->data.begin() + 2048, 'X');
    
    // Read 1: 100 bytes (Fits in 128)
    char buf[2048];
    conveyor_read(conv, buf, 100);
    
    // Read 2: 100 bytes (Sequential) -> Heuristic counter ++
    conveyor_read(conv, buf, 100);
    
    // Read 3: 100 bytes (Sequential) -> Heuristic counter ++
    conveyor_read(conv, buf, 100);
    
    // Read 4: 1000 bytes. 
    // This is larger than current capacity (128). 
    // Should trigger IMMEDIATE resize to accommodate.
    ssize_t res = conveyor_read(conv, buf, 1000);
    
    ASSERT_EQ(res, 1000);
    
    // Verify we got correct data
    // (If resize failed or offset calc was wrong, this would be garbage)
    for(int i=0; i<1000; i++) ASSERT_EQ(buf[i], 'X');
    
    conveyor_destroy(conv);
}
