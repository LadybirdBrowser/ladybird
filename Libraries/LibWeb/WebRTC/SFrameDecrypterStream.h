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

class SFrameDecrypterStream final : public DOM::EventTarget {
    WEB_WRAPPABLE(SFrameDecrypterStream, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SFrameDecrypterStream);

public:
    static GC::Ref<SFrameDecrypterStream> create();
    static WebIDL::ExceptionOr<GC::Ref<SFrameDecrypterStream>> construct_impl(Bindings::SFrameTransformOptions const&);
    virtual ~SFrameDecrypterStream() override;

private:
    explicit SFrameDecrypterStream();
};

}
