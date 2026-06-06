/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Encoding/TextDecoder.h>
#include <LibWeb/Encoding/TextDecoderCommon.h>
#include <LibWeb/Streams/GenericTransformStream.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecoderstream
class TextDecoderStream final
    : public Bindings::Wrappable
    , public Streams::GenericTransformStreamMixin
    , public TextDecoderCommonMixin {
    WEB_WRAPPABLE(TextDecoderStream, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TextDecoderStream);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextDecoderStream>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, FlyString label, Bindings::TextDecoderOptions const&);
    virtual ~TextDecoderStream() override;

private:
    TextDecoderStream(GC::Ref<Streams::TransformStream>, TextCodec::Decoder&, FlyString encoding, ErrorMode, bool ignore_bom);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> decode_and_enqueue_chunk(JS::Realm&, JS::Value);
    WebIDL::ExceptionOr<void> flush_and_enqueue(JS::Realm&);

    WebIDL::ExceptionOr<void> enqueue_decoded_output(JS::Realm&, String const&);

    // https://encoding.spec.whatwg.org/#textdecodercommon-i-o-queue
    // NB: We accumulate input bytes that have been pushed to the I/O queue but not yet decoded, so that a multi-byte
    //     sequence which is split across chunks can be reassembled.
    ByteBuffer m_io_queue;
};

}
