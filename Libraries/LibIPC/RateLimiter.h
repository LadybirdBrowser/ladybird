/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Types.h>

namespace IPC {

// Token bucket rate limiter for IPC messages
// Prevents denial-of-service attacks through message flooding
//
// Usage:
//   RateLimiter limiter(1000, Duration::from_milliseconds(10));
//   if (limiter.try_consume()) {
//       process_message();
//   } else {
//       // Rate limit exceeded - reject or disconnect
//   }
class RateLimiter {
public:
    // Create rate limiter with maximum tokens and refill interval
    // Example: RateLimiter(1000, Duration::from_milliseconds(10))
    //   -> 1000 messages per second (100 tokens refilled every 10ms)
    RateLimiter(size_t max_tokens, Duration refill_interval)
        : m_max_tokens(max_tokens)
        , m_tokens(max_tokens)
        , m_refill_interval(refill_interval)
        , m_last_refill(MonotonicTime::now())
    {
    }

    // Try to consume tokens; returns true if allowed, false if rate limit exceeded
    [[nodiscard]] bool try_consume(size_t count = 1)
    {
        refill();

        if (m_tokens >= count) {
            m_tokens -= count;
            return true;
        }

        return false;
    }

    // Get current token count (for monitoring/debugging)
    [[nodiscard]] size_t tokens() const { return m_tokens; }

    // Get maximum token capacity
    [[nodiscard]] size_t max_tokens() const { return m_max_tokens; }

    // Reset to full capacity
    void reset()
    {
        m_tokens = m_max_tokens;
        m_last_refill = MonotonicTime::now();
    }

    // Check if rate limit would be exceeded without consuming
    [[nodiscard]] bool would_exceed(size_t count = 1) const
    {
        // Note: This doesn't refill, so it's a conservative estimate
        return m_tokens < count;
    }

    // Get time until next token refill
    [[nodiscard]] Duration time_until_refill() const
    {
        auto now = MonotonicTime::now();
        auto elapsed = now - m_last_refill;

        if (elapsed >= m_refill_interval)
            return Duration::zero();

        return m_refill_interval - elapsed;
    }

private:
    void refill()
    {
        auto now = MonotonicTime::now();
        auto elapsed = now - m_last_refill;

        if (elapsed >= m_refill_interval) {
            // Calculate how many refill intervals have passed
            auto intervals = elapsed.to_nanoseconds() / m_refill_interval.to_nanoseconds();

            // Refill tokens (capped at max)
            m_tokens = min(m_tokens + static_cast<size_t>(intervals), m_max_tokens);

            // Update last refill time
            m_last_refill = now;
        }
    }

    size_t m_max_tokens;
    size_t m_tokens;
    Duration m_refill_interval;
    MonotonicTime m_last_refill;
};

// Adaptive rate limiter that adjusts limits based on behavior
// Starts permissive but becomes stricter if abuse is detected
class AdaptiveRateLimiter {
public:
    AdaptiveRateLimiter(size_t initial_max_tokens, Duration refill_interval)
        : m_base_limiter(initial_max_tokens, refill_interval)
        , m_initial_max_tokens(initial_max_tokens)
        , m_consecutive_violations(0)
    {
    }

    [[nodiscard]] bool try_consume(size_t count = 1)
    {
        if (m_base_limiter.try_consume(count)) {
            // Success - reset violation counter
            if (m_consecutive_violations > 0) {
                m_consecutive_violations--;
            }
            return true;
        }

        // Rate limit exceeded
        m_consecutive_violations++;

        // After multiple violations, reduce capacity (more strict)
        if (m_consecutive_violations >= 5) {
            // Reduce to 50% capacity
            auto new_max = m_initial_max_tokens / 2;
            if (new_max > 0) {
                m_base_limiter = RateLimiter(new_max, Duration::from_milliseconds(10));
            }
        }

        return false;
    }

    void reset()
    {
        m_base_limiter = RateLimiter(m_initial_max_tokens, Duration::from_milliseconds(10));
        m_consecutive_violations = 0;
    }

    [[nodiscard]] size_t consecutive_violations() const
    {
        return m_consecutive_violations;
    }

private:
    RateLimiter m_base_limiter;
    size_t m_initial_max_tokens;
    size_t m_consecutive_violations;
};

}
