/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::WebRTC {

class RTCDtlsTransport final : public DOM::EventTarget {
    WEB_WRAPPABLE(RTCDtlsTransport, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(RTCDtlsTransport);

public:
    [[nodiscard]] static GC::Ref<RTCDtlsTransport> create();
    virtual ~RTCDtlsTransport() override;

    void set_onstatechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onstatechange();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

private:
    RTCDtlsTransport();
};

}
