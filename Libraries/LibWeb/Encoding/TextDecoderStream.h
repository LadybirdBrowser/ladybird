/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Encoding/TextDecoder.h>
#include <LibWeb/Encoding/TextDecoderCommon.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/GenericTransformStream.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecoderstream
class TextDecoderStream final
    : public Bindings::GCAllocatedWrappable
    , public Streams::GenericTransformStreamMixin
    , public TextDecoderCommonMixin {
    WEB_WRAPPABLE(TextDecoderStream, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(TextDecoderStream);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextDecoderStream>> create_for_constructor(JS::Object&, Utf16String const& label, TextDecoderOptions const&);
    virtual ~TextDecoderStream() override;

private:
    TextDecoderStream(GC::Ref<Streams::TransformStream>, FlyString encoding, TextCodec::ErrorMode, bool ignore_bom);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> decode_and_enqueue_chunk(JS::VM&, JS::Value);
    WebIDL::ExceptionOr<void> flush_and_enqueue(JS::VM&);

    WebIDL::ExceptionOr<void> enqueue_decoded_output(JS::VM&, Utf16String const&);

    // https://encoding.spec.whatwg.org/#textdecodercommon-i-o-queue
    // NB: We accumulate input bytes that have been pushed to the I/O queue but not yet decoded, so that a multi-byte
    //     sequence which is split across chunks can be reassembled.
};

}
