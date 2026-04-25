/*
 * Copyright (c) 2026-present, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>

class UBiDi;
typedef unsigned char UBiDiLevel;

namespace Unicode {

class BidiParagraph {
public:
    static NonnullOwnPtr<BidiParagraph> create(AK::Utf16View const& text, UBiDiLevel embedding_level);
    ~BidiParagraph();

    UBiDiLevel get_bidi_level_at(size_t index);

private:
    BidiParagraph(AK::Utf16View const& text, UBiDiLevel embedding_level);

    UBiDi* m_bidi_paragraph { nullptr };
};

}
