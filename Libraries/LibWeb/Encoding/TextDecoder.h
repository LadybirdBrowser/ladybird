/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Encoding/TextDecoderCommon.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecoder
class TextDecoder
    : public Bindings::Wrappable
    , public TextDecoderCommonMixin {
    WEB_WRAPPABLE(TextDecoder, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TextDecoder);

public:
    static WebIDL::ExceptionOr<GC::Ref<TextDecoder>> create(String const& label, TextDecoderOptions const& options = {});
    static WebIDL::ExceptionOr<GC::Ref<TextDecoder>> create(FlyString encoding, TextDecoderOptions const& options = {});

    virtual ~TextDecoder() override;

    WebIDL::ExceptionOr<String> decode(Optional<WebIDL::BufferSourceVariant> const& input, Optional<TextDecodeOptions> const&) const;
    WebIDL::ExceptionOr<String> decode(Optional<ReadonlyBytes>) const;

private:
    TextDecoder(TextCodec::Decoder&, FlyString encoding, ErrorMode error_mode, bool ignore_bom);
};

}
