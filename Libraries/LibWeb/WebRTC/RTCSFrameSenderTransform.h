/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebRTC {

class RTCSFrameSenderTransform final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(RTCSFrameSenderTransform, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(RTCSFrameSenderTransform);

public:
    static GC::Ref<RTCSFrameSenderTransform> create();
    static WebIDL::ExceptionOr<GC::Ref<RTCSFrameSenderTransform>> construct_impl(Bindings::RTCSFrameSenderTransformOptions const&);
    virtual ~RTCSFrameSenderTransform() override;

private:
    explicit RTCSFrameSenderTransform();
};

}
