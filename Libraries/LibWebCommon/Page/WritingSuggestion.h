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

// A predicted continuation of the text around the caret: The text replaces the UTF-16 code units
// [replacement_start, replacement_start + replacement_length) of the text that was sent.
struct WritingSuggestion {
    u32 replacement_start { 0 };
    u32 replacement_length { 0 };
    Utf16String text;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::WritingSuggestion const&);

template<>
WEBCOMMON_API ErrorOr<Web::WritingSuggestion> decode(Decoder&);

}
