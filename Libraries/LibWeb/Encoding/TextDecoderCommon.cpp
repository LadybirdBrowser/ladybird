/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Encoding/TextDecoderCommon.h>

namespace Web::Encoding {

TextDecoderCommonMixin::TextDecoderCommonMixin(TextCodec::Decoder& decoder, FlyString encoding, ErrorMode error_mode, bool ignore_bom)
    : m_decoder(decoder)
    , m_encoding(move(encoding))
    , m_error_mode(error_mode)
    , m_ignore_bom(ignore_bom)
{
}

TextDecoderCommonMixin::~TextDecoderCommonMixin() = default;

}
