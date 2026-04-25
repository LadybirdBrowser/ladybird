/*
 * Copyright (c) 2026-present, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <AK/Utf16StringData.h>
#include <LibUnicode/Bidi.h>
#include <LibUnicode/ICU.h>

#include <unicode/ubidi.h>
#include <unicode/unistr.h>

namespace Unicode {

NonnullOwnPtr<BidiParagraph> BidiParagraph::create(AK::Utf16View const& text, UBiDiLevel embedding_level)
{
    return adopt_own(*new BidiParagraph { text, embedding_level });
}

BidiParagraph::BidiParagraph(AK::Utf16View const& text, UBiDiLevel embedding_level)
{
    auto icu_string = Unicode::icu_string(text);

    m_bidi_paragraph = ubidi_open();

    UErrorCode status = U_ZERO_ERROR;
    ubidi_setPara(
        m_bidi_paragraph,
        icu_string.getBuffer(),
        icu_string.length(),
        embedding_level,
        nullptr,
        &status);
    verify_icu_success(status);
}

BidiParagraph::~BidiParagraph()
{
    ubidi_close(m_bidi_paragraph);
}

UBiDiLevel BidiParagraph::get_bidi_level_at(size_t index)
{
    return ubidi_getLevelAt(m_bidi_paragraph, index);
}

}
