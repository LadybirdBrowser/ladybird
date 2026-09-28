/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/HTML/AutoplayPolicy.h>

namespace Web::HTML {

Optional<AutoplayPolicy> autoplay_policy_from_string(Utf16View string)
{
    if (string == u"allow-audio-and-video"sv)
        return AutoplayPolicy::AllowAudioAndVideo;
    if (string == u"block-audio"sv)
        return AutoplayPolicy::BlockAudio;
    if (string == u"block-audio-and-video"sv)
        return AutoplayPolicy::BlockAudioAndVideo;
    return {};
}

Utf16View autoplay_policy_to_string(AutoplayPolicy policy)
{
    switch (policy) {
    case AutoplayPolicy::AllowAudioAndVideo:
        return u"allow-audio-and-video"sv;
    case AutoplayPolicy::BlockAudio:
        return u"block-audio"sv;
    case AutoplayPolicy::BlockAudioAndVideo:
        return u"block-audio-and-video"sv;
    }

    VERIFY_NOT_REACHED();
}

}
