/*
 * Copyright (c) 2025, Saksham Mittal <hi@gotlou.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <LibJS/Forward.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Encoding/TextDecoderCommon.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/GenericTransformStream.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecoderstream
class TextDecoderStream final
    : public Bindings::PlatformObject
    , public Streams::GenericTransformStreamMixin
    , public Encoding::TextDecoderCommonMixin {
    WEB_PLATFORM_OBJECT(TextDecoderStream, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TextDecoderStream);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextDecoderStream>> construct_impl(JS::Realm&, FlyString encoding_label, TextDecoderOptions const& options);

    virtual ~TextDecoderStream() override;

    WebIDL::ExceptionOr<void> decode_and_enqueue_chunk(JS::Value chunk);
    WebIDL::ExceptionOr<void> flush_and_enqueue();

private:
    TextDecoderStream(JS::Realm&, TextCodec::Decoder&, FlyString encoding, bool fatal, bool ignore_bom, GC::Ref<Streams::TransformStream> transform);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    WebIDL::ExceptionOr<String> serialize_io_queue(Vector<u32> const& queue);

    TextCodec::Decoder& m_decoder;
    Vector<ByteBuffer> m_io_queue;
    bool m_do_not_flush { false };
};

}
