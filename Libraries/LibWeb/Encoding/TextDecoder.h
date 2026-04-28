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
#include <LibWeb/Encoding/TextDecoderCommon.h>
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
class TextDecoder
    : public Bindings::PlatformObject
    , public TextDecoderCommonMixin {
    WEB_PLATFORM_OBJECT(TextDecoder, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TextDecoder);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextDecoder>> construct_impl(JS::Realm&, FlyString encoding, Optional<TextDecoderOptions> const& options = {});

    virtual ~TextDecoder() override;

    WebIDL::ExceptionOr<String> decode(Optional<GC::Root<WebIDL::BufferSource>> const&, Optional<TextDecodeOptions> const& options = {}) const;

private:
    TextDecoder(JS::Realm&, TextCodec::Decoder&, FlyString encoding, ErrorMode error_mode, bool ignore_bom);

    virtual void initialize(JS::Realm&) override;
};

}
