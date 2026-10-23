/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NumericLimits.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Weakable.h>
#include <LibRequests/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>

namespace Web::Fetch::Engine {

// The transport may deliver only up to the smaller of the channel's credit and the navigation hold. Every method
// runs on the loop the gate was created on.
class WEB_API FetchBodyDeliveryGate final : public AtomicRefCounted<FetchBodyDeliveryGate> {
public:
    enum class NavigationHold : u8 {
        None,
        Held,
    };

    class WEB_API Transport : public RefCounted<Transport>
        , public Weakable<Transport> {
    public:
        virtual ~Transport() = default;
        virtual void resume_body_delivery_up_to(u64 byte_count) = 0;
        virtual void pause_body_delivery() = 0;
        virtual void stop() = 0;

        // Called for a delivery no honest transport could have made.
        virtual void misbehaved(StringView reason);
    };

    [[nodiscard]] static NonnullRefPtr<FetchBodyDeliveryGate> create(NavigationHold);
    ~FetchBodyDeliveryGate();

    [[nodiscard]] NonnullRefPtr<FetchByteChannelProducerSink> create_channel_sink();

    // Allowances granted before the transport exists are applied on attach.
    void attach_transport(NonnullRefPtr<Transport>);
    void attach_transport(NonnullRefPtr<Requests::Request>);

    enum class DeliveryResult : u8 {
        Accepted,
        Rejected,
    };

    // Delivery consumes both allowances; a delivery past them is reported to the transport and rejected.
    [[nodiscard]] DeliveryResult did_deliver(u64 byte_count);

    // Permits this many further bytes from the current delivery point.
    void release_hold_up_to(u64 byte_count);
    void release_hold();

    void consumer_cancelled();

    // A detached transport is never touched again.
    void detach_transport();

    void grant_credit(u64 byte_count);

private:
    explicit FetchBodyDeliveryGate(NavigationHold);

    static constexpr u64 unbounded = NumericLimits<u64>::max();

    void apply_to_transport();

    u64 m_channel_allowance { 0 };
    u64 m_hold_allowance { 0 };
    u64 m_delivered_byte_count { 0 };
    bool m_terminated { false };
    RefPtr<Transport> m_transport;
};

}
