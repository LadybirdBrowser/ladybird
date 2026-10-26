/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RTCIceCandidate.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

using Bindings::RTCIceCandidateInit;

class RTCIceCandidate final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCIceCandidate, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCIceCandidate);

public:
    [[nodiscard]] static GC::Ref<RTCIceCandidate> create(RTCIceCandidateInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<RTCIceCandidate>> construct_impl(RTCIceCandidateInit const& = {});

    virtual ~RTCIceCandidate() override;

    Utf16String const& candidate() const { return m_init.candidate; }
    Optional<Utf16String> sdp_mid() const { return m_init.sdp_mid; }
    Optional<u16> sdp_m_line_index() const { return m_init.sdp_m_line_index; }
    Optional<Utf16String> username_fragment() const { return m_init.username_fragment; }
    RTCIceCandidateInit to_json() const { return m_init; }
    Optional<Utf16String> foundation() const { return {}; }
    Optional<Bindings::RTCIceServerTransportProtocol> relay_protocol() const { return {}; }

private:
    explicit RTCIceCandidate(RTCIceCandidateInit);
    RTCIceCandidateInit m_init;
};

}
