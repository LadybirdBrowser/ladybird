/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

enum class AutoplayPolicy : u8 {
    AllowAudioAndVideo,
    BlockAudio,
    BlockAudioAndVideo,
};

WEB_API Optional<AutoplayPolicy> autoplay_policy_from_string(StringView);
WEB_API StringView autoplay_policy_to_string(AutoplayPolicy);

}
