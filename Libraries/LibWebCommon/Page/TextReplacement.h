/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web {

// A replacement the user set up for a word they typed, such as a macOS Text Replacement: The text replaces the UTF-16
// code units [start, start + length) of the text that was sent.
struct TextReplacement {
    u32 start { 0 };
    u32 length { 0 };
    Utf16String text;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::TextReplacement const&);

template<>
WEBCOMMON_API ErrorOr<Web::TextReplacement> decode(Decoder&);

}
