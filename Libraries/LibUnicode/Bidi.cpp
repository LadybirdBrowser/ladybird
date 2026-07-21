/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Bidi.h>
#include <LibUnicode/CharacterTypes.h>

namespace Unicode {

bool may_require_bidi_processing(Utf16View const& text)
{
    if (text.has_ascii_storage())
        return false;

    for (auto code_point : text) {
        switch (bidirectional_class(code_point)) {
        case BidiClass::RightToLeft:
        case BidiClass::RightToLeftArabic:
        case BidiClass::RightToLeftEmbedding:
        case BidiClass::RightToLeftIsolate:
        case BidiClass::RightToLeftOverride:
        case BidiClass::LeftToRightEmbedding:
        case BidiClass::LeftToRightIsolate:
        case BidiClass::LeftToRightOverride:
        case BidiClass::FirstStrongIsolate:
        case BidiClass::PopDirectionalFormat:
        case BidiClass::PopDirectionalIsolate:
            return true;
        default:
            break;
        }
    }

    return false;
}

}
