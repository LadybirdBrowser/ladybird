/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibRequests/Request.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>

namespace Web::Fetch::Engine {

// Controls the RequestServer request a fetch's response arrives through. Every method runs on the loop the handle
// was created on.
class WEB_API NetworkTransportHandle final : public FetchBodyDeliveryGate::Transport {
public:
    [[nodiscard]] static NonnullRefPtr<NetworkTransportHandle> create(NonnullRefPtr<Requests::Request>);

    // The key another client adopts the request by, while this process holds its lease.
    [[nodiscard]] Optional<Requests::RequestTransferLeaseKey> const& transfer_lease() const { return m_request->transfer_lease(); }
    [[nodiscard]] ByteString const& http_version() const { return m_request->http_version(); }

    [[nodiscard]] bool has_file_backed_response_body() const;

    virtual void resume_body_delivery_up_to(u64 byte_count) override;
    virtual void pause_body_delivery() override;
    virtual void stop() override;

    void release_transfer_lease();

    void set_on_requires_network(Function<void()>);
    void set_on_teardown(Function<void(Requests::Request::TeardownReason)>);

private:
    explicit NetworkTransportHandle(NonnullRefPtr<Requests::Request>);

    NonnullRefPtr<Requests::Request> m_request;
};

}
