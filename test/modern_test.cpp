#include <gtest/gtest.h>
#include "mock_storage.hpp"
#include "libconveyor/conveyor_modern.hpp"

TEST(ModernApiTest, VectorWriteAndRead) {
    MockStorage mock(4096);
    auto ops = mock.get_ops();
    
    libconveyor::v2::Config cfg;
    cfg.handle = (storage_handle_t)&mock;
    cfg.ops = ops;
    
    // 1. Factory
    auto res = libconveyor::v2::Conveyor::create(cfg);
    ASSERT_TRUE(res);
    auto conveyor = std::move(res.value());
    
    // 2. Vector Write
    std::vector<int> numbers = {1, 2, 3, 4};
    auto write_res = conveyor.write(numbers);
    ASSERT_TRUE(write_res);
    ASSERT_EQ(write_res.value(), 4 * sizeof(int));
    
    conveyor.flush();
    
    // 3. Verify Mock
    int* mock_ints = reinterpret_cast<int*>(mock.data.data());
    EXPECT_EQ(mock_ints[0], 1);
    EXPECT_EQ(mock_ints[3], 4);
    
    // 4. Seek
    auto seek_res = conveyor.seek(0);
    ASSERT_TRUE(seek_res);
    
    // 5. Vector Read
    std::vector<int> read_back(4);
    auto read_res = conveyor.read(read_back);
    ASSERT_TRUE(read_res);
    EXPECT_EQ(read_back[0], 1);
    EXPECT_EQ(read_back[3], 4);
}
