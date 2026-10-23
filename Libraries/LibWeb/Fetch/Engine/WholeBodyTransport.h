/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibCore/ImmutableBytes.h>
#include <LibRequests/Request.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>

namespace Web::Fetch::Engine {

// Delivers a body that is available all at once, in pieces no larger than the gate allows. Every method runs on the
// loop the transport was created on.
class WEB_API WholeBodyTransport final : public FetchBodyDeliveryGate::Transport {
public:
    using OnData = Function<void(Requests::ResponseData)>;

    [[nodiscard]] static NonnullRefPtr<WholeBodyTransport> create();

    void attach_to(FetchBodyDeliveryGate&);
    [[nodiscard]] bool is_attached() const { return m_attached; }

    void set_body(Core::ImmutableBytes, OnData);

    // Runs once every byte of the body has been delivered, or never if the transport stops first.
    void when_delivered(Function<void()>);

    // Runs once if the transport stops before delivering the whole body.
    void set_on_stop(Function<void()>);

    virtual void resume_body_delivery_up_to(u64 byte_count) override;
    virtual void pause_body_delivery() override;
    virtual void stop() override;

private:
    WholeBodyTransport() = default;

    void schedule_delivery();
    void deliver();

    Optional<Core::ImmutableBytes> m_body;
    OnData m_on_data;
    Function<void()> m_on_delivered;
    Function<void()> m_on_stop;
    size_t m_delivered_byte_count { 0 };
    u64 m_allowance { 0 };
    bool m_attached { false };
    bool m_delivery_scheduled { false };
    bool m_stopped { false };
};

}
