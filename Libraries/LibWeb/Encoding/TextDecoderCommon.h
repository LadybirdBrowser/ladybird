/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibTextCodec/Decoder.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecodercommon
class TextDecoderCommonMixin {
public:
    virtual ~TextDecoderCommonMixin();

    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    FlyString const& encoding() const { return m_encoding; }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-fatal
    bool fatal() const { return m_error_mode == TextCodec::ErrorMode::Fatal; }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-ignorebom
    bool ignore_bom() const { return m_ignore_bom; }

protected:
    TextDecoderCommonMixin(TextCodec::Decoder& decoder, FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom);

    // https://encoding.spec.whatwg.org/#textdecodercommon-decoder
    TextCodec::Decoder& m_decoder;

    // https://encoding.spec.whatwg.org/#textdecoder-encoding
    FlyString m_encoding;

    // https://encoding.spec.whatwg.org/#textdecoder-error-mode
    TextCodec::ErrorMode m_error_mode { TextCodec::ErrorMode::Replacement };

    // https://encoding.spec.whatwg.org/#textdecoder-ignore-bom-flag
    bool m_ignore_bom { false };
};

}
