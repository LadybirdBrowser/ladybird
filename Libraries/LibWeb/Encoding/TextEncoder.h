/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Encoding/TextEncoderCommon.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#dictdef-textencoderencodeintoresult
struct TextEncoderEncodeIntoResult {
    WebIDL::UnsignedLongLong read;
    WebIDL::UnsignedLongLong written;
};

// https://encoding.spec.whatwg.org/#textencoder
class TextEncoder final
    : public Bindings::PlatformObject
    , public TextEncoderCommonMixin {
    WEB_PLATFORM_OBJECT(TextEncoder, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TextEncoder);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextEncoder>> construct_impl(JS::Realm&);

    virtual ~TextEncoder() override;

    GC::Ref<JS::Uint8Array> encode(String const& input) const;
    TextEncoderEncodeIntoResult encode_into(String const& source, GC::Root<WebIDL::BufferSource> const& destination) const;

protected:
    // https://encoding.spec.whatwg.org/#dom-textencoder
    explicit TextEncoder(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
