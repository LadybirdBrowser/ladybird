/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Utf16View.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

enum class AutoplayPolicy : u8 {
    AllowAudioAndVideo,
    BlockAudio,
    BlockAudioAndVideo,
};

WEB_API Optional<AutoplayPolicy> autoplay_policy_from_string(Utf16View);
WEB_API Utf16View autoplay_policy_to_string(AutoplayPolicy);

}
