/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibTextCodec/Forward.h>

namespace Web::Encoding {

// https://encoding.spec.whatwg.org/#textdecodercommon
class TextDecoderCommonMixin {
public:
    virtual ~TextDecoderCommonMixin();

    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    FlyString const& encoding() const { return m_encoding; }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-fatal
    bool fatal() const { return m_error_mode == ErrorMode::Fatal; }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-ignorebom
    bool ignore_bom() const { return m_ignore_bom; }

protected:
    // https://encoding.spec.whatwg.org/#concept-encoding-error-mode
    enum class ErrorMode {
        Replacement,
        Fatal,
    };

    TextDecoderCommonMixin(TextCodec::Decoder& decoder, FlyString encoding, ErrorMode error_mode, bool ignore_bom);

    // https://encoding.spec.whatwg.org/#textdecodercommon-decoder
    TextCodec::Decoder& m_decoder;

    // https://encoding.spec.whatwg.org/#textdecoder-encoding
    FlyString m_encoding;

    // https://encoding.spec.whatwg.org/#textdecoder-error-mode
    ErrorMode m_error_mode { ErrorMode::Replacement };

    // https://encoding.spec.whatwg.org/#textdecoder-ignore-bom-flag
    bool m_ignore_bom { false };

    // https://encoding.spec.whatwg.org/#textdecoder-bom-seen-flag
    bool m_bom_seen { false };
};

}
