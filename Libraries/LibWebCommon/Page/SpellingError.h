/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web {

// A misspelled run of a checked text, in UTF-16 code units.
struct SpellingError {
    u32 offset { 0 };
    u32 length { 0 };

    bool operator==(SpellingError const&) const = default;
};

// The page context menu's spelling items a test can ask the UI process to activate.
enum class SpellingMenuActionForTesting : u8 {
    None,
    Guess,
    IgnoreSpelling,
    LearnSpelling,
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::SpellingError const&);

template<>
WEBCOMMON_API ErrorOr<Web::SpellingError> decode(Decoder&);

}
