/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/SFrameDecrypterStream.h>
#include <LibWeb/WebRTC/SFrameDecrypterStream.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(SFrameDecrypterStream);

GC::Ref<SFrameDecrypterStream> SFrameDecrypterStream::create() { return GC::Heap::the().allocate<SFrameDecrypterStream>(); }

// FIXME: SFrameDecrypterStream is a stub — wire up the options once SFrame transforms are implemented.
WebIDL::ExceptionOr<GC::Ref<SFrameDecrypterStream>> SFrameDecrypterStream::construct_impl(Bindings::SFrameTransformOptions const&)
{
    return create();
}
SFrameDecrypterStream::SFrameDecrypterStream() = default;
SFrameDecrypterStream::~SFrameDecrypterStream() = default;

}
