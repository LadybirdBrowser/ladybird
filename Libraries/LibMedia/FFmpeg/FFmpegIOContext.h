/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/FFmpeg/FFmpegForward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

namespace Media::FFmpeg {

class FFmpegIOContext {
public:
    explicit FFmpegIOContext(NonnullRefPtr<IncrementallyPopulatedStream::Seekable>, AVIOContext*);
    ~FFmpegIOContext();

    static ErrorOr<NonnullOwnPtr<FFmpegIOContext>> create(NonnullRefPtr<IncrementallyPopulatedStream::Seekable> stream);

    AVIOContext* avio_context() const { return m_avio_context; }

    void do_blocked_reads() const
    {
        m_stream->set_blocking_read(true);
    }

private:
    NonnullRefPtr<IncrementallyPopulatedStream::Seekable> m_stream;
    AVIOContext* m_avio_context { nullptr };
};

}
