/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RTCSessionDescription.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

using Bindings::RTCLocalSessionDescriptionInit;
using Bindings::RTCSessionDescriptionInit;

class RTCSessionDescription final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCSessionDescription, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCSessionDescription);

public:
    [[nodiscard]] static GC::Ref<RTCSessionDescription> create(RTCSessionDescriptionInit const&);
    static WebIDL::ExceptionOr<GC::Ref<RTCSessionDescription>> construct_impl(RTCSessionDescriptionInit const&);

    virtual ~RTCSessionDescription() override;

    Bindings::RTCSdpType type() const { return m_type; }
    Utf16String const& sdp() const { return m_sdp; }
    RTCSessionDescriptionInit to_json() const { return { .sdp = m_sdp, .type = m_type }; }

private:
    RTCSessionDescription(Bindings::RTCSdpType, Utf16String);

    Bindings::RTCSdpType m_type {};
    Utf16String m_sdp;
};

}
