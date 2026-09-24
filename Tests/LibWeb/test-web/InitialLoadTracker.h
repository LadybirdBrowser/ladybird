/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>

class InitialLoadTracker {
public:
    explicit InitialLoadTracker(size_t view_count)
        : m_view_count(view_count)
    {
    }

    void mark_ready(size_t view_index)
    {
        VERIFY(view_index < m_view_count);
        m_ready_views.set(view_index);
    }

    size_t ready_count() const { return m_ready_views.size(); }
    bool all_ready() const { return ready_count() == m_view_count; }

private:
    size_t m_view_count;
    HashTable<size_t> m_ready_views;
};
