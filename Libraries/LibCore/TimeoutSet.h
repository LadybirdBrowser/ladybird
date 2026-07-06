/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/BinaryHeap.h>
#include <AK/NumericLimits.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>

namespace Core {

class TimeoutSet;

class EventLoopTimeout {
public:
    static constexpr ssize_t INVALID_INDEX = NumericLimits<ssize_t>::max();

    EventLoopTimeout() { }
    virtual ~EventLoopTimeout() = default;

    virtual void fire(TimeoutSet& timeout_set, MonotonicTime time) = 0;

    MonotonicTime fire_time() const { return m_fire_time; }

    void absolutize(Badge<TimeoutSet>, MonotonicTime current_time)
    {
        m_fire_time = current_time + m_duration;
    }

    ssize_t& index(Badge<TimeoutSet>) { return m_index; }
    void set_index(Badge<TimeoutSet>, ssize_t index) { m_index = index; }

    bool is_scheduled() const { return m_index != INVALID_INDEX; }

    void set_sequence_id(u64 id) { m_sequence_id = id; }
    u64 sequence_id() const { return m_sequence_id; }

protected:
    union {
        AK::Duration m_duration;
        MonotonicTime m_fire_time;
    };

private:
    ssize_t m_index = INVALID_INDEX;
    u64 m_sequence_id { 0 };
};

class TimeoutSet {
public:
    TimeoutSet() = default;

    Optional<MonotonicTime> next_timer_expiration()
    {
        if (!m_heap.is_empty()) {
            return m_heap.peek_min()->fire_time();
        } else {
            return {};
        }
    }

    void absolutize_relative_timeouts(MonotonicTime current_time)
    {
        for (auto timeout : m_scheduled_timeouts) {
            timeout->absolutize({}, current_time);
            m_heap.insert(timeout);
        }
        m_scheduled_timeouts.clear();
    }

    size_t fire_expired(MonotonicTime current_time)
    {
        size_t fired_count = 0;
        while (!m_heap.is_empty()) {
            auto& timeout = *m_heap.peek_min();

            if (timeout.fire_time() <= current_time) {
                ++fired_count;
                m_heap.pop_min();
                timeout.set_index({}, EventLoopTimeout::INVALID_INDEX);
                timeout.fire(*this, current_time);
            } else {
                break;
            }
        }
        return fired_count;
    }

    void schedule_relative(EventLoopTimeout* timeout)
    {
        timeout->set_sequence_id(m_next_sequence_id++);
        timeout->set_index({}, -1 - static_cast<ssize_t>(m_scheduled_timeouts.size()));
        m_scheduled_timeouts.append(timeout);
    }

    void schedule_absolute(EventLoopTimeout* timeout)
    {
        timeout->set_sequence_id(m_next_sequence_id++);
        m_heap.insert(timeout);
    }

    void unschedule(EventLoopTimeout* timeout)
    {
        if (timeout->index({}) < 0) {
            size_t i = -1 - timeout->index({});
            size_t j = m_scheduled_timeouts.size() - 1;
            VERIFY(m_scheduled_timeouts[i] == timeout);
            swap(m_scheduled_timeouts[i], m_scheduled_timeouts[j]);
            swap(m_scheduled_timeouts[i]->index({}), m_scheduled_timeouts[j]->index({}));
            (void)m_scheduled_timeouts.take_last();
        } else {
            m_heap.pop(timeout->index({}));
        }
        timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
    }

    void clear()
    {
        for (auto* timeout : m_heap.nodes_in_arbitrary_order())
            timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
        m_heap.clear();
        for (auto* timeout : m_scheduled_timeouts)
            timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
        m_scheduled_timeouts.clear();
    }

private:
    struct TimeoutFiresBefore {
        bool operator()(EventLoopTimeout* a, EventLoopTimeout* b) const
        {
            if (a->fire_time() == b->fire_time())
                return a->sequence_id() < b->sequence_id();
            return a->fire_time() < b->fire_time();
        }
    };

    struct UpdateTimeoutIndex {
        void operator()(EventLoopTimeout* timeout, size_t index) const
        {
            timeout->set_index({}, static_cast<ssize_t>(index));
        }
    };

    IntrusiveBinaryHeap<EventLoopTimeout*, TimeoutFiresBefore, UpdateTimeoutIndex, 8> m_heap;
    Vector<EventLoopTimeout*, 8> m_scheduled_timeouts;
    u64 m_next_sequence_id { 0 };
};

}
