/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace Media::FFmpeg {

class FFmpegIOContext {
public:
    explicit FFmpegIOContext(AVIOContext*);
    ~FFmpegIOContext();

    static ErrorOr<NonnullOwnPtr<FFmpegIOContext>> create(AK::SeekableStream& stream);

    AVIOContext* avio_context() const { return m_avio_context; }

private:
    AVIOContext* m_avio_context { nullptr };
};

}
