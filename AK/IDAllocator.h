/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashTable.h>
#include <AK/Random.h>

namespace AK {

// This class manages a pool of random ID's in the range N (default of 1) to INT32_MAX
class IDAllocator {

public:
    IDAllocator() = default;

    explicit IDAllocator(int minimum_value)
        : m_minimum_value(minimum_value)
    {
    }

    ~IDAllocator() = default;

    int allocate()
    {
        VERIFY(m_allocated_ids.size() < (INT32_MAX - 2));
        int id = 0;
        for (;;) {
            id = static_cast<int>(get_random_uniform(NumericLimits<int>::max()));
            if (id < m_minimum_value)
                continue;
            if (m_allocated_ids.set(id) == AK::HashSetResult::InsertedNewEntry)
                break;
        }
        return id;
    }

    void deallocate(int id)
    {
        m_allocated_ids.remove(id);
    }

private:
    HashTable<int> m_allocated_ids;
    int m_minimum_value { 1 };
};

}

#if USING_AK_GLOBALLY
using AK::IDAllocator;
#endif
