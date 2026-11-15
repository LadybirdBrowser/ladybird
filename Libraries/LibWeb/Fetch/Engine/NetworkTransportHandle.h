/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/ThreadID.h>
#include <LibRequests/Request.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/Ids.h>

namespace Web::Fetch::Engine {

class FetchExecutor;

// Controls the RequestServer request a fetch's response arrives through. Every method, and the handle's destruction,
// runs on the loop the handle was created on.
class WEB_API NetworkTransportHandle final : public FetchBodyDeliveryGate::Transport {
public:
    [[nodiscard]] static NonnullRefPtr<NetworkTransportHandle> create(NonnullRefPtr<Requests::Request>);
    virtual ~NetworkTransportHandle() override;

    [[nodiscard]] RequestId request_id() const { return RequestId { m_request->id() }; }
    [[nodiscard]] TransportEpoch transport_epoch() const { return TransportEpoch { m_request->transport_epoch() }; }

    // The key another client adopts the request by, while this process holds its lease.
    [[nodiscard]] Optional<Requests::RequestTransferLeaseKey> const& transfer_lease() const { return m_request->transfer_lease(); }
    [[nodiscard]] ByteString const& http_version() const { return m_request->http_version(); }

    [[nodiscard]] bool has_file_backed_response_body() const;

    virtual void resume_body_delivery_up_to(u64 byte_count) override;
    virtual void pause_body_delivery() override;
    virtual void stop() override;

    void release_transfer_lease();

    // The agent's controls, which reach the transport through the given executor. A stop is requested from the moment
    // it is posted.
    void post_stop(FetchExecutor&);
    [[nodiscard]] bool is_stop_requested() const { return m_stop_requested; }
    void post_release_navigation_hold(FetchExecutor&);
    void post_release_navigation_hold_up_to(FetchExecutor&, u64 byte_count);

    // Runs in the first post_stop(), unless it was taken before.
    void set_on_stop_requested(Function<void()>);
    [[nodiscard]] Function<void()> take_on_stop_requested();

    void set_on_requires_network(Function<void()>);
    void set_on_upload_progress(Function<void(u64 bytes_sent)>);
    void set_on_teardown(Function<void(Requests::Request::TeardownReason)>);

private:
    explicit NetworkTransportHandle(NonnullRefPtr<Requests::Request>);

    [[nodiscard]] bool is_on_owner_thread() const { return m_owner_thread.is_current_thread(); }
    void post_transport_control(FetchExecutor&, auto operation);

    NonnullRefPtr<Requests::Request> m_request;
    AK::ThreadID m_owner_thread { AK::ThreadID::current() };
    bool m_stop_requested { false };
    Function<void()> m_on_stop_requested;
};

}
