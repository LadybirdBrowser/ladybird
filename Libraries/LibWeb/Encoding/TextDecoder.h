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

// https://encoding.spec.whatwg.org/#textdecoder
class TextDecoder final
    : public Bindings::PlatformObject
    , public Encoding::TextDecoderCommonMixin {
    WEB_PLATFORM_OBJECT(TextDecoder, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TextDecoder);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextDecoder>> construct_impl(JS::Realm&, FlyString encoding, Optional<TextDecoderOptions> const& options = {});

    virtual ~TextDecoder() override;

    WebIDL::ExceptionOr<String> decode(Optional<GC::Root<WebIDL::BufferSource>> const&, Optional<TextDecodeOptions> const& options = {}) const;

private:
    TextDecoder(JS::Realm&, TextCodec::Decoder&, FlyString encoding, bool fatal, bool ignore_bom);

    virtual void initialize(JS::Realm&) override;

    TextCodec::Decoder& m_decoder;
};

}
