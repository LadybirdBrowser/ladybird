/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::WebRTC {

class RTCSctpTransport final : public DOM::EventTarget {
    WEB_WRAPPABLE(RTCSctpTransport, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(RTCSctpTransport);

public:
    [[nodiscard]] static GC::Ref<RTCSctpTransport> create();
    virtual ~RTCSctpTransport() override;

    void set_onstatechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onstatechange();

private:
    RTCSctpTransport();
};

}
