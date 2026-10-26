/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/SFrameEncrypterStream.h>
#include <LibWeb/WebRTC/SFrameEncrypterStream.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(SFrameEncrypterStream);

GC::Ref<SFrameEncrypterStream> SFrameEncrypterStream::create() { return GC::Heap::the().allocate<SFrameEncrypterStream>(); }

// FIXME: SFrameEncrypterStream is a stub — wire up the options once SFrame transforms are implemented.
WebIDL::ExceptionOr<GC::Ref<SFrameEncrypterStream>> SFrameEncrypterStream::construct_impl(Bindings::SFrameTransformOptions const&)
{
    return create();
}
SFrameEncrypterStream::SFrameEncrypterStream() = default;
SFrameEncrypterStream::~SFrameEncrypterStream() = default;

}
