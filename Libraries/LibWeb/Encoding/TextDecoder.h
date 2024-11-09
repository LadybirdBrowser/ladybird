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
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecoderoptions
struct TextDecoderOptions {
    bool fatal = false;
    bool ignore_bom = false;
};

// https://encoding.spec.whatwg.org/#textdecodeoptions
struct TextDecodeOptions {
    bool stream = false;
};

// https://encoding.spec.whatwg.org/#textdecoder
class TextDecoder : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TextDecoder, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(TextDecoder);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<TextDecoder>> construct_impl(JS::Realm&, FlyString encoding, Optional<TextDecoderOptions> const& options = {});

    virtual ~TextDecoder() override;

    WebIDL::ExceptionOr<String> decode(Optional<JS::Handle<WebIDL::BufferSource>> const&, Optional<TextDecodeOptions> const& options = {}) const;

    FlyString const& encoding() const { return m_encoding; }
    bool fatal() const { return m_fatal; }
    bool ignore_bom() const { return m_ignore_bom; }

private:
    TextDecoder(JS::Realm&, TextCodec::Decoder&, FlyString encoding, bool fatal, bool ignore_bom);

    virtual void initialize(JS::Realm&) override;

    TextCodec::Decoder& m_decoder;
    FlyString m_encoding;
    bool m_fatal { false };
    bool m_ignore_bom { false };
};

}
