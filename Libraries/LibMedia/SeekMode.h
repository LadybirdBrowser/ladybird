/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace Media {

enum class SeekMode : u8 {
    Accurate,
    FastBefore,
    FastAfter,
};

constexpr StringView seek_mode_to_string(SeekMode seek_mode)
{
    switch (seek_mode) {
    case SeekMode::Accurate:
        return "Accurate"sv;
    case SeekMode::FastBefore:
        return "FastBefore"sv;
    case SeekMode::FastAfter:
        return "FastAfter"sv;
    }
    VERIFY_NOT_REACHED();
}

}

namespace AK {

template<>
struct Formatter<Media::SeekMode> final : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::SeekMode color_primaries)
    {
        return Formatter<StringView>::format(builder, Media::seek_mode_to_string(color_primaries));
    }
};

}
