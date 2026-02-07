/*
 * Copyright (c) 2025, Saksham Mittal <hi@gotlou.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

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

// https://encoding.spec.whatwg.org/#interface-mixin-textdecodercommon
class TextDecoderCommonMixin {
public:
    virtual ~TextDecoderCommonMixin();

    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    FlyString const& encoding() const { return m_encoding; }
    // https://encoding.spec.whatwg.org/#dom-textdecoder-fatal
    bool fatal() const { return m_fatal; }
    // https://encoding.spec.whatwg.org/#dom-textdecoder-ignorebom
    bool ignore_bom() const { return m_ignore_bom; }
    // https://encoding.spec.whatwg.org/#textdecoder-bom-seen-flag
    bool bom_seen() const { return m_bom_seen; }

protected:
    TextDecoderCommonMixin();
    TextDecoderCommonMixin(FlyString encoding, bool fatal, bool ignore_bom)
        : m_encoding(move(encoding))
        , m_fatal(fatal)
        , m_ignore_bom(ignore_bom)
    {
    }

    FlyString m_encoding;
    bool m_fatal { false };
    bool m_ignore_bom { false };
    bool m_bom_seen { false };
};

}
