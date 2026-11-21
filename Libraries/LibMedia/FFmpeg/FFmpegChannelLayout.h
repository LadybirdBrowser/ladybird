/*
 * Copyright (c) 2024-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibAudio/ChannelMap.h>
#include <LibMedia/Export.h>
#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>

extern "C" {
#include <libavutil/channel_layout.h>
}

namespace Media::FFmpeg {

class MEDIA_API FFmpegChannelLayout {
    AK_MAKE_NONCOPYABLE(FFmpegChannelLayout);

public:
    FFmpegChannelLayout(FFmpegChannelLayout&& other)
        : m_functions(exchange(other.m_functions, nullptr))
        , m_channel_layout(exchange(other.m_channel_layout, {}))
    {
    }

    ~FFmpegChannelLayout()
    {
        if (m_functions)
            m_functions->av_channel_layout_uninit(&m_channel_layout);
    }

    FFmpegChannelLayout& operator=(FFmpegChannelLayout&& other)
    {
        if (this == &other)
            return *this;
        if (m_functions)
            m_functions->av_channel_layout_uninit(&m_channel_layout);
        m_functions = exchange(other.m_functions, nullptr);
        m_channel_layout = exchange(other.m_channel_layout, {});
        return *this;
    }

    ErrorOr<Audio::ChannelMap> to_channel_map() const;
    static ErrorOr<FFmpegChannelLayout> from_channel_map(FFmpegFunctions const&, Audio::ChannelMap const&);
    static FFmpegChannelLayout from_opt(FFmpegFunctions const&, void* obj, char const* name);

    AVChannelLayout const& layout() { return m_channel_layout; }
    AVChannelLayout leak_layout()
    {
        m_functions = nullptr;
        return exchange(m_channel_layout, {});
    }

private:
    FFmpegChannelLayout(FFmpegFunctions const& functions, AVChannelLayout const& layout)
        : m_functions(&functions)
        , m_channel_layout(layout)
    {
    }

    FFmpegFunctions const* m_functions;
    AVChannelLayout m_channel_layout;
};

}
