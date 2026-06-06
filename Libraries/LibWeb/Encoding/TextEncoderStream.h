/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Encoding/TextEncoderCommon.h>
#include <LibWeb/Streams/GenericTransformStream.h>

namespace Web::Encoding {

class TextEncoderStream final
    : public Bindings::Wrappable
    , public Streams::GenericTransformStreamMixin
    , public TextEncoderCommonMixin {
    WEB_WRAPPABLE(TextEncoderStream, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TextEncoderStream);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextEncoderStream>> construct_impl(JS::Realm&);
    virtual ~TextEncoderStream() override;

private:
    TextEncoderStream(JS::Realm&, GC::Ref<Streams::TransformStream>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> encode_and_enqueue_chunk(JS::Value);
    WebIDL::ExceptionOr<void> encode_and_flush();

    Optional<u32> convert_code_unit_to_scalar_value(u32 item, Utf8CodePointIterator& code_point_iterator);

    // https://encoding.spec.whatwg.org/#textencoderstream-pending-high-surrogate
    Optional<u32> m_leading_surrogate;
};

}
