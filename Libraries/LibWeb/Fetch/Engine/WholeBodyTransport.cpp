/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/Fetch/Engine/WholeBodyTransport.h>

namespace Web::Fetch::Engine {

NonnullRefPtr<WholeBodyTransport> WholeBodyTransport::create()
{
    return adopt_ref(*new WholeBodyTransport);
}

void WholeBodyTransport::attach_to(FetchBodyDeliveryGate& gate)
{
    VERIFY(!m_attached);
    m_attached = true;
    gate.attach_transport(*this);
}

void WholeBodyTransport::set_body(Core::ImmutableBytes body, OnData on_data)
{
    VERIFY(m_attached);
    VERIFY(!m_body.has_value());
    m_body = move(body);
    m_on_data = move(on_data);
    schedule_delivery();
}

void WholeBodyTransport::when_delivered(Function<void()> on_delivered)
{
    VERIFY(!m_on_delivered);
    m_on_delivered = move(on_delivered);
    schedule_delivery();
}

void WholeBodyTransport::set_on_stop(Function<void()> on_stop)
{
    m_on_stop = move(on_stop);
}

void WholeBodyTransport::resume_body_delivery_up_to(u64 byte_count)
{
    m_allowance = byte_count;
    schedule_delivery();
}

void WholeBodyTransport::pause_body_delivery()
{
    m_allowance = 0;
}

void WholeBodyTransport::stop()
{
    m_stopped = true;
    m_body.clear();
    m_on_data = nullptr;
    m_on_delivered = nullptr;
    if (auto on_stop = move(m_on_stop))
        on_stop();
}

void WholeBodyTransport::schedule_delivery()
{
    if (m_stopped || m_delivery_scheduled)
        return;

    m_delivery_scheduled = true;
    Core::deferred_invoke([self = NonnullRefPtr(*this)] {
        self->m_delivery_scheduled = false;
        self->deliver();
    });
}

void WholeBodyTransport::deliver()
{
    if (!m_body.has_value())
        return;

    auto body_size = m_body->size();
    while (!m_stopped && m_delivered_byte_count < body_size && m_allowance > 0) {
        auto offset = m_delivered_byte_count;
        auto length = static_cast<size_t>(min<u64>(body_size - offset, m_allowance));
        m_delivered_byte_count += length;
        m_allowance -= length;
        m_on_data(Requests::ResponseData::from_file_backed_payload(*m_body, offset, length));
    }

    if (m_stopped || m_delivered_byte_count < body_size)
        return;

    m_on_data = nullptr;
    m_on_stop = nullptr;
    if (auto on_delivered = move(m_on_delivered))
        on_delivered();
}

}
