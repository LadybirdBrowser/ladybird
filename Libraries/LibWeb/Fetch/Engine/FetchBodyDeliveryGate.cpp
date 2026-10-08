/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>

namespace Web::Fetch::Engine {

// A command posted after the gate's loop is gone is dropped.
class FetchBodyDeliveryGateSink final : public FetchByteChannelProducerSink {
public:
    explicit FetchBodyDeliveryGateSink(NonnullRefPtr<FetchBodyDeliveryGate> gate)
        : m_producer_loop(Core::EventLoop::current_weak())
        , m_gate(move(gate))
    {
    }

    virtual void grant_credit(u64 byte_count) override
    {
        post([byte_count](FetchBodyDeliveryGate& gate) { gate.grant_credit(byte_count); });
    }

    virtual void consumer_cancelled() override
    {
        post([](FetchBodyDeliveryGate& gate) { gate.consumer_cancelled(); });
    }

private:
    void post(Function<void(FetchBodyDeliveryGate&)> command)
    {
        auto loop = m_producer_loop->take();
        if (!loop)
            return;
        loop->deferred_invoke([gate = m_gate, command = move(command)] { command(*gate); });
    }

    NonnullRefPtr<Core::WeakEventLoopReference> m_producer_loop;
    NonnullRefPtr<FetchBodyDeliveryGate> m_gate;
};

NonnullRefPtr<FetchBodyDeliveryGate> FetchBodyDeliveryGate::create(NavigationHold navigation_hold)
{
    return adopt_ref(*new FetchBodyDeliveryGate(navigation_hold));
}

FetchBodyDeliveryGate::FetchBodyDeliveryGate(NavigationHold navigation_hold)
    : m_hold_allowance(navigation_hold == NavigationHold::Held ? 0 : unbounded)
{
}

FetchBodyDeliveryGate::~FetchBodyDeliveryGate()
{
    VERIFY(!m_transport);
}

NonnullRefPtr<FetchByteChannelProducerSink> FetchBodyDeliveryGate::create_channel_sink()
{
    return adopt_ref(*new FetchBodyDeliveryGateSink(*this));
}

void FetchBodyDeliveryGate::attach_transport(NonnullRefPtr<Transport> transport)
{
    if (m_terminated) {
        transport->stop();
        return;
    }
    m_transport = move(transport);
    apply_to_transport();
}

void FetchBodyDeliveryGate::Transport::misbehaved(StringView reason)
{
    dbgln("FetchBodyDeliveryGate: The transport misbehaved: {}", reason);
    VERIFY_NOT_REACHED();
}

FetchBodyDeliveryGate::DeliveryResult FetchBodyDeliveryGate::did_deliver(u64 byte_count)
{
    auto allowance = min(m_channel_allowance, m_hold_allowance);
    if (byte_count > allowance - min(allowance, m_delivered_byte_count)) {
        if (m_transport)
            m_transport->misbehaved("The transport delivered more than it was allowed"sv);
        return DeliveryResult::Rejected;
    }
    m_delivered_byte_count += byte_count;
    return DeliveryResult::Accepted;
}

void FetchBodyDeliveryGate::release_hold_up_to(u64 byte_count)
{
    if (m_hold_allowance == unbounded)
        return;
    m_hold_allowance = max(m_hold_allowance, m_delivered_byte_count + byte_count);
    apply_to_transport();
}

void FetchBodyDeliveryGate::release_hold()
{
    m_hold_allowance = unbounded;
    apply_to_transport();
}

void FetchBodyDeliveryGate::grant_credit(u64 byte_count)
{
    m_channel_allowance += byte_count;
    apply_to_transport();
}

void FetchBodyDeliveryGate::consumer_cancelled()
{
    m_terminated = true;
    if (auto transport = exchange(m_transport, nullptr))
        transport->stop();
}

void FetchBodyDeliveryGate::detach_transport()
{
    m_terminated = true;
    m_transport = nullptr;
}

void FetchBodyDeliveryGate::release_transport()
{
    if (auto transport = exchange(m_transport, nullptr))
        transport->pause_body_delivery();
    m_terminated = true;
}

void FetchBodyDeliveryGate::apply_to_transport()
{
    if (!m_transport)
        return;

    auto allowance = min(m_channel_allowance, m_hold_allowance);
    auto remaining = allowance - min(allowance, m_delivered_byte_count);
    if (remaining > 0)
        m_transport->resume_body_delivery_up_to(remaining);
    else
        m_transport->pause_body_delivery();
}

}
