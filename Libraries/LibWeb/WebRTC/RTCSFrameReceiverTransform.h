/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

class RTCSFrameReceiverTransform final : public DOM::EventTarget {
    WEB_WRAPPABLE(RTCSFrameReceiverTransform, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(RTCSFrameReceiverTransform);

public:
    static GC::Ref<RTCSFrameReceiverTransform> create();
    static WebIDL::ExceptionOr<GC::Ref<RTCSFrameReceiverTransform>> construct_impl(Bindings::SFrameTransformOptions const&);
    virtual ~RTCSFrameReceiverTransform() override;

private:
    explicit RTCSFrameReceiverTransform();
};

}
