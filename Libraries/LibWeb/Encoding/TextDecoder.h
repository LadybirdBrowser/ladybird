/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <LibJS/Forward.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TextDecoder.h>
#include <LibWeb/Encoding/TextDecoderCommon.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecoder
class TextDecoder
    : public Bindings::PlatformObject
    , public TextDecoderCommonMixin {
    WEB_PLATFORM_OBJECT(TextDecoder, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TextDecoder);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextDecoder>> construct_impl(JS::Realm&, StringView label, Bindings::TextDecoderOptions const&);

    virtual ~TextDecoder() override;

    WebIDL::ExceptionOr<String> decode(Optional<WebIDL::BufferSourceVariant>, Bindings::TextDecodeOptions const&);

private:
    TextDecoder(JS::Realm&, FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom);

    virtual void initialize(JS::Realm&) override;

    // https://encoding.spec.whatwg.org/#textdecoder-do-not-flush-flag
    // A TextDecoder object has an associated do not flush, which is a boolean, initially false.
    bool m_do_not_flush { false };
};

}
