/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::WebRTC {

class RTCDTMFSender final : public DOM::EventTarget {
    WEB_WRAPPABLE(RTCDTMFSender, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(RTCDTMFSender);

public:
    [[nodiscard]] static GC::Ref<RTCDTMFSender> create();
    virtual ~RTCDTMFSender() override;

    void set_ontonechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* ontonechange();

private:
    RTCDTMFSender();
};

}
