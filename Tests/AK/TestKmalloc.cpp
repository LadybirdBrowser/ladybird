/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

TEST_CASE(allocating_from_every_partition_on_several_threads)
{
    static constexpr Array partitions {
        HeapPartition::General,
        HeapPartition::ArrayBuffer,
        HeapPartition::JSObjectStorage,
        HeapPartition::Layout,
        HeapPartition::String,
    };

    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < 8; ++i) {
        auto thread = Threading::Thread::construct("PartitionAllocator"sv, [i]() {
            auto fill = static_cast<u8>(i + 1);
            Vector<Bytes> allocations;
            allocations.ensure_capacity(1000);
            for (size_t round = 0; round < 200; ++round) {
                for (size_t j = 0; j < 1000; ++j) {
                    auto partition = partitions[j % partitions.size()];
                    auto size = 16 + (j % 64) * 8;
                    auto* allocation = static_cast<u8*>(kmalloc(partition, size));
                    VERIFY(allocation);
                    __builtin_memset(allocation, fill, size);
                    allocations.unchecked_append({ allocation, size });
                }
                for (auto allocation : allocations) {
                    EXPECT_EQ(allocation[0], fill);
                    EXPECT_EQ(allocation[allocation.size() - 1], fill);
                    kfree(allocation.data());
                }
                allocations.clear_with_capacity();
            }
            return 0;
        });
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        (void)thread->join();
}
