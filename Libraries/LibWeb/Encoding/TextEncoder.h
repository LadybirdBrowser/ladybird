/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Encoding/TextEncoderCommon.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Bindings {

struct TextEncoderEncodeIntoResult;

}

namespace Web::Encoding {

struct EncodeIntoResult {
    WebIDL::UnsignedLongLong read { 0 };
    WebIDL::UnsignedLongLong written { 0 };
};

// https://encoding.spec.whatwg.org/#textencoder
class TextEncoder final
    : public Bindings::Wrappable
    , public TextEncoderCommonMixin {
    WEB_WRAPPABLE(TextEncoder, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TextEncoder);

public:
    [[nodiscard]] static GC::Ref<TextEncoder> create();

    virtual ~TextEncoder() override;

    ErrorOr<ByteBuffer> encode_to_byte_buffer(String const& input) const;
    WebIDL::ExceptionOr<GC::Ref<JS::Uint8Array>> encode(JS::Realm&, String const& input) const;
    EncodeIntoResult encode_into_result(String const& source, GC::Root<JS::Uint8Array> const& destination) const;
    Bindings::TextEncoderEncodeIntoResult encode_into(JS::Realm&, String const& source, GC::Root<JS::Uint8Array> const& destination) const;

protected:
    // https://encoding.spec.whatwg.org/#dom-textencoder
    explicit TextEncoder();
};

}
