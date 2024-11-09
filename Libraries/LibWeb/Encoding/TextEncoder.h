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
class TextEncoder final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TextEncoder, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(TextEncoder);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<TextEncoder>> construct_impl(JS::Realm&);

    virtual ~TextEncoder() override;

    JS::NonnullGCPtr<JS::Uint8Array> encode(String const& input) const;
    TextEncoderEncodeIntoResult encode_into(String const& source, JS::Handle<WebIDL::BufferSource> const& destination) const;

    static FlyString const& encoding();

protected:
    // https://encoding.spec.whatwg.org/#dom-textencoder
    explicit TextEncoder(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
