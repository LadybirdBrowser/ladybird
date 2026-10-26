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

class SFrameEncrypterStream final : public DOM::EventTarget {
    WEB_WRAPPABLE(SFrameEncrypterStream, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SFrameEncrypterStream);

public:
    static GC::Ref<SFrameEncrypterStream> create();
    static WebIDL::ExceptionOr<GC::Ref<SFrameEncrypterStream>> construct_impl(Bindings::SFrameTransformOptions const&);
    virtual ~SFrameEncrypterStream() override;

private:
    explicit SFrameEncrypterStream();
};

}
