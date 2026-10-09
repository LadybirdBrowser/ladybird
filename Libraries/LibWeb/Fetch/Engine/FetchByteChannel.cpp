/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Engine/FetchByteChannel.h>

namespace Web::Fetch::Engine {

NonnullRefPtr<FetchByteChannel> FetchByteChannel::create(Watermarks watermarks, NonnullRefPtr<FetchByteChannelProducerSink> producer_sink)
{
    return adopt_ref(*new FetchByteChannel(watermarks, move(producer_sink)));
}

FetchByteChannel::FetchByteChannel(Watermarks watermarks, NonnullRefPtr<FetchByteChannelProducerSink> producer_sink)
    : m_watermarks(watermarks)
    , m_producer_sink(move(producer_sink))
    , m_outstanding_transport_credit(watermarks.high)
{
    VERIFY(m_watermarks.low > 0);
    VERIFY(m_watermarks.low <= m_watermarks.high);
}

// A sink may reenter the channel.
void FetchByteChannel::dispatch(PendingNotifications const& notifications)
{
    for (auto const& notification : notifications) {
        notification.visit(
            [&](Wake const& wake) { m_consumer_sink->wake(wake.ticket); },
            [&](GrantCredit const& grant) { m_producer_sink->grant_credit(grant.byte_count); },
            [&](ConsumerCancelledNotice) { m_producer_sink->consumer_cancelled(); },
            [&](Misbehaved const& misbehaved) { m_producer_sink->misbehaved(misbehaved.reason); });
    }
}

void FetchByteChannelProducerSink::misbehaved(StringView reason)
{
    dbgln("FetchByteChannel: The transport misbehaved: {}", reason);
    VERIFY_NOT_REACHED();
}

bool FetchByteChannel::is_due_locked(WakeOn const& wake_on) const
{
    if (!m_state.has<Open>())
        return true;
    return wake_on.visit(
        [&](BufferedAtLeast const& buffered_at_least) { return m_buffered_byte_count >= buffered_at_least.byte_count; },
        [](EndOnly) { return false; });
}

void FetchByteChannel::wake_due_waiters_locked(PendingNotifications& notifications)
{
    m_armed_waiters.remove_all_matching([&](ArmedWaiter const& waiter) {
        if (!is_due_locked(waiter.wake_on))
            return false;
        notifications.append(Wake { waiter.ticket });
        return true;
    });
}

FetchByteChannel::WriteResult FetchByteChannel::write(Core::ImmutableBytes bytes)
{
    return write_bytes(move(bytes));
}

FetchByteChannel::WriteResult FetchByteChannel::write_copy(ReadonlyBytes bytes)
{
    return write_bytes(bytes);
}

// This implements the buffer-filling steps of HTTP-network fetch's transmission loop.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
FetchByteChannel::WriteResult FetchByteChannel::write_bytes(Variant<Core::ImmutableBytes, ReadonlyBytes> bytes)
{
    auto size = bytes.visit([](auto const& written) -> u64 { return written.size(); });

    PendingNotifications notifications;
    auto result = WriteResult::Rejected;
    {
        MutexLocker locker { m_mutex };
        if (m_transport_ended) {
            notifications.append(Misbehaved { "The transport delivered after ending the body"sv });
        } else if (!m_state.has<Open>()) {
            // The consumer cancelled and the transport has not heard yet.
        } else if (size > m_outstanding_transport_credit) {
            notifications.append(Misbehaved { "The transport delivered more than it was granted"sv });
        } else {
            result = WriteResult::Accepted;
            if (size > 0) {
                // 12. Append bytes to buffer.
                m_buffered_byte_count += size;
                bytes.visit(
                    [&](Core::ImmutableBytes& chunk) {
                        seal_copied_tail_locked();
                        m_chunks.append(move(chunk));
                    },
                    [&](ReadonlyBytes copied) { m_copied_tail.append(copied); });

                // 13. If the size of buffer is larger than an upper limit chosen by the user agent, ask the user agent
                //     to suspend the ongoing fetch.
                // Spent credit is the suspension: the transport delivers only what it was granted.
                m_outstanding_transport_credit -= size;
                wake_due_waiters_locked(notifications);
            }
        }
    }
    dispatch(notifications);
    return result;
}

void FetchByteChannel::seal_copied_tail_locked()
{
    if (m_copied_tail.is_empty())
        return;
    m_chunks.append(Core::ImmutableBytes::adopt(move(m_copied_tail)));
    m_copied_tail = {};
}

FetchByteChannel::TerminateResult FetchByteChannel::terminate_locked(State terminal_state, PendingNotifications& notifications)
{
    VERIFY(!terminal_state.has<Open>());
    if (!m_state.has<Open>())
        return TerminateResult::AlreadyTerminated;

    if (terminal_state.has<Errored>()) {
        m_chunks.clear();
        m_copied_tail.clear();
        m_buffered_byte_count = 0;
    }
    m_state = move(terminal_state);
    wake_due_waiters_locked(notifications);
    return TerminateResult::Terminated;
}

FetchByteChannel::TerminateResult FetchByteChannel::close()
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        MutexLocker locker { m_mutex };
        if (m_transport_ended) {
            notifications.append(Misbehaved { "The transport ended the body twice"sv });
        } else {
            m_transport_ended = true;
            result = terminate_locked(Closed {}, notifications);
        }
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TerminateResult FetchByteChannel::error(Errored errored)
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        MutexLocker locker { m_mutex };
        if (m_transport_ended) {
            notifications.append(Misbehaved { "The transport ended the body twice"sv });
        } else {
            m_transport_ended = true;
            result = terminate_locked(move(errored), notifications);
        }
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TerminateResult FetchByteChannel::cancel()
{
    PendingNotifications notifications;
    auto result = TerminateResult::AlreadyTerminated;
    {
        MutexLocker locker { m_mutex };
        // A Closed channel keeps its undrained buffer, which a Streams cancellation may still discard.
        if (m_state.has<Open>() || (m_state.has<Closed>() && m_buffered_byte_count > 0)) {
            m_state = ConsumerCancelled {};
            m_chunks.clear();
            m_copied_tail.clear();
            m_buffered_byte_count = 0;
            wake_due_waiters_locked(notifications);
            if (!m_transport_ended)
                notifications.append(ConsumerCancelledNotice {});
            result = TerminateResult::Terminated;
        }
    }
    dispatch(notifications);
    return result;
}

Vector<Core::ImmutableBytes> FetchByteChannel::transfer_out()
{
    PendingNotifications notifications;
    Vector<Core::ImmutableBytes> undrained_bytes;
    {
        MutexLocker locker { m_mutex };
        if (!m_state.has<Open>() && !m_state.has<Closed>())
            return {};

        seal_copied_tail_locked();
        undrained_bytes = move(m_chunks);
        m_buffered_byte_count = 0;
        m_state = Transferred {};
        wake_due_waiters_locked(notifications);
    }
    dispatch(notifications);
    return undrained_bytes;
}

// This implements the buffer-draining half of the pullAlgorithm in HTTP-network fetch.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
FetchByteChannel::TakeResult FetchByteChannel::take_locked(u64 limit, PendingNotifications& notifications)
{
    seal_copied_tail_locked();

    TakeResult result;

    u64 remaining = limit;
    while (remaining > 0 && !m_chunks.is_empty()) {
        auto& front = m_chunks.first();
        auto available = front.size();
        auto length = static_cast<size_t>(min<u64>(available, remaining));

        result.taken.append(front.slice(0, length));
        remaining -= length;
        m_buffered_byte_count -= length;

        if (length == available)
            m_chunks.take_first();
        else
            front = front.slice(length, available - length);
    }

    result.state = m_state;
    result.buffered_byte_count = m_buffered_byte_count;

    // 1. If the size of buffer is smaller than a lower limit chosen by the user agent and the
    //    ongoing fetch is suspended, resume the fetch.
    if (m_state.has<Open>() && m_buffered_byte_count < m_watermarks.low) {
        auto target = m_watermarks.high - m_buffered_byte_count;
        if (target > m_outstanding_transport_credit) {
            notifications.append(GrantCredit { target - m_outstanding_transport_credit });
            m_outstanding_transport_credit = target;
        }
    }

    return result;
}

void FetchByteChannel::attach_consumer(NonnullRefPtr<FetchByteChannelConsumerSink> consumer_sink)
{
    VERIFY(!m_consumer_sink);
    m_consumer_sink = move(consumer_sink);
}

FetchByteChannel::TakeResult FetchByteChannel::take_up_to(u64 limit)
{
    PendingNotifications notifications;
    TakeResult result;
    {
        MutexLocker locker { m_mutex };
        result = take_locked(limit, notifications);
    }
    dispatch(notifications);
    return result;
}

FetchByteChannel::TakeResult FetchByteChannel::take_all()
{
    return take_up_to(NumericLimits<u64>::max());
}

FetchByteChannel::TakeResult FetchByteChannel::peek_up_to(u64 limit)
{
    MutexLocker locker { m_mutex };
    seal_copied_tail_locked();

    TakeResult result;
    u64 remaining = limit;
    for (auto const& chunk : m_chunks) {
        if (remaining == 0)
            break;
        auto length = static_cast<size_t>(min<u64>(chunk.size(), remaining));
        result.taken.append(chunk.slice(0, length));
        remaining -= length;
    }
    result.state = m_state;
    result.buffered_byte_count = m_buffered_byte_count;
    return result;
}

WakeTicket FetchByteChannel::arm_waiter(WakeOn wake_on)
{
    VERIFY(m_consumer_sink);
    wake_on.visit(
        [](BufferedAtLeast const& buffered_at_least) { VERIFY(buffered_at_least.byte_count > 0); },
        [](EndOnly) { });

    PendingNotifications notifications;
    WakeTicket ticket { 0 };
    {
        MutexLocker locker { m_mutex };
        ticket = WakeTicket { m_next_ticket++ };
        if (is_due_locked(wake_on))
            notifications.append(Wake { ticket });
        else
            m_armed_waiters.append({ ticket, move(wake_on) });
    }
    dispatch(notifications);
    return ticket;
}

void FetchByteChannel::disarm_waiter(WakeTicket ticket)
{
    MutexLocker locker { m_mutex };
    m_armed_waiters.remove_first_matching([&](ArmedWaiter const& waiter) { return waiter.ticket == ticket; });
}

}
