// MIT License
//
// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <iostream>
#include <numeric>
#include <vector>
#include <type_traits>

// Google Test
#include <gtest/gtest.h>
// HC API
#include <hcc/hc.hpp>
// rocPRIM
#include <block/block_exchange.hpp>
#include <block/block_load.hpp>
#include <block/block_store.hpp>

#include "test_utils.hpp"

namespace rp = rocprim;

template<
    class T,
    class U,
    unsigned int BlockSize,
    unsigned int ItemsPerThread
>
struct params
{
    using type = T;
    using output_type = U;
    static constexpr unsigned int block_size = BlockSize;
    static constexpr unsigned int items_per_thread = ItemsPerThread;
};

template<class Params>
class RocprimBlockExchangeTests : public ::testing::Test {
public:
    using params = Params;
};

template<class T>
struct dummy
{
    T x;
    T y;

    dummy() [[hc]] [[cpu]]
    { }

    template<class U>
    dummy(U a) [[hc]] [[cpu]]
        : x(a + 1), y(a * 2)
    { }

    bool operator==(const dummy &b) const { return x == b.x && y == b.y; }
};

typedef ::testing::Types<
// params<int, int, 128, 4>

    // Power of 2 BlockSize and ItemsPerThread = 1 (no rearrangement)
    params<int, long long, 64, 1>,
    params<unsigned long long, unsigned long long, 128, 1>,
    params<short, dummy<int>, 256, 1>,
    params<long long, long long, 512, 1>,

    // Power of 2 BlockSize and ItemsPerThread > 1
    params<int, int, 512, 5>,
    params<short, dummy<float>, 128, 7>,
    params<int, int, 128, 3>,
    params<unsigned long long, unsigned long long, 64, 3>,

    // Non-power of 2 BlockSize and ItemsPerThread > 1
    params<int, double, 33U, 5>,
    params<char, dummy<double>, 464U, 2>,
    params<unsigned short, unsigned int, 100U, 3>,
    params<short, int, 234U, 9>
> Params;

TYPED_TEST_CASE(RocprimBlockExchangeTests, Params);

TYPED_TEST(RocprimBlockExchangeTests, BlockedToStriped)
{
    hc::accelerator acc;

    using type = typename TestFixture::params::type;
    using output_type = typename TestFixture::params::output_type;
    constexpr size_t block_size = TestFixture::params::block_size;
    constexpr size_t items_per_thread = TestFixture::params::items_per_thread;
    constexpr size_t items_per_block = block_size * items_per_thread;
    // Given block size not supported
    if(block_size > get_max_tile_size(acc))
    {
        return;
    }

    const size_t size = items_per_block * 113;
    // Generate data
    std::vector<type> input(size);
    std::iota(input.begin(), input.end(), 0);
    std::vector<output_type> output(size, output_type(0));

    // Calculate expected results on host
    std::vector<output_type> expected(size);
    for(size_t bi = 0; bi < size / items_per_block; bi++)
    {
        for(size_t ti = 0; ti < block_size; ti++)
        {
            for(size_t ii = 0; ii < items_per_thread; ii++)
            {
                const size_t src = bi * items_per_block + ii * block_size + ti;
                const size_t dst = bi * items_per_block + ti * items_per_thread + ii;
                expected[dst] = input[src];
            }
        }
    }

    hc::array_view<type, 1> d_input(size, input.data());
    hc::array_view<output_type, 1> d_output(size, output.data());
    hc::parallel_for_each(
        acc.get_default_view(),
        hc::extent<1>(size / items_per_thread).tile(block_size),
        [=](hc::tiled_index<1> idx) [[hc]]
        {
            const unsigned int lid = idx.local[0];
            const unsigned int block_offset = idx.tile[0] * items_per_block;

            type input[items_per_thread];
            output_type output[items_per_thread];
            rp::block_load_direct_blocked(lid, d_input.data() + block_offset, input);

            rp::block_exchange<type, block_size, items_per_thread> exchange;
            exchange.blocked_to_striped(input, output);

            rp::block_store_direct_blocked(lid, d_output.data() + block_offset, output);
        }
    );

    d_output.synchronize();
    for(int i = 0; i < size; i++)
    {
        ASSERT_EQ(output[i], expected[i]);
    }
}

TYPED_TEST(RocprimBlockExchangeTests, StripedToBlocked)
{
    hc::accelerator acc;

    using type = typename TestFixture::params::type;
    using output_type = typename TestFixture::params::output_type;
    constexpr size_t block_size = TestFixture::params::block_size;
    constexpr size_t items_per_thread = TestFixture::params::items_per_thread;
    constexpr size_t items_per_block = block_size * items_per_thread;
    // Given block size not supported
    if(block_size > get_max_tile_size(acc))
    {
        return;
    }

    const size_t size = items_per_block * 113;
    // Generate data
    std::vector<type> input(size);
    std::iota(input.begin(), input.end(), 0);
    std::vector<output_type> output(size, output_type(0));

    // Calculate expected results on host
    std::vector<output_type> expected(size);

    for(size_t bi = 0; bi < size / items_per_block; bi++)
    {
        for(size_t ti = 0; ti < block_size; ti++)
        {
            for(size_t ii = 0; ii < items_per_thread; ii++)
            {
                const size_t dst = bi * items_per_block + ii * block_size + ti;
                const size_t src = bi * items_per_block + ti * items_per_thread + ii;
                expected[dst] = input[src];
            }
        }
    }

    hc::array_view<type, 1> d_input(size, input.data());
    hc::array_view<output_type, 1> d_output(size, output.data());
    hc::parallel_for_each(
        acc.get_default_view(),
        hc::extent<1>(size / items_per_thread).tile(block_size),
        [=](hc::tiled_index<1> idx) [[hc]]
        {
            const unsigned int lid = idx.local[0];
            const unsigned int block_offset = idx.tile[0] * items_per_block;

            type input[items_per_thread];
            output_type output[items_per_thread];
            rp::block_load_direct_blocked(lid, d_input.data() + block_offset, input);

            rp::block_exchange<type, block_size, items_per_thread> exchange;
            exchange.striped_to_blocked(input, output);

            rp::block_store_direct_blocked(lid, d_output.data() + block_offset, output);
        }
    );

    d_output.synchronize();
    for(int i = 0; i < size; i++)
    {
        ASSERT_EQ(output[i], expected[i]);
    }
}
