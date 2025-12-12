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
    explicit FFmpegIOContext(NonnullRefPtr<IncrementallyPopulatedStream::Cursor>, AVIOContext*);
    ~FFmpegIOContext();

    static ErrorOr<NonnullOwnPtr<FFmpegIOContext>> create(NonnullRefPtr<IncrementallyPopulatedStream::Cursor>);

    AVIOContext* avio_context() const { return m_avio_context; }

private:
    NonnullRefPtr<IncrementallyPopulatedStream::Cursor> m_stream_cursor;
    AVIOContext* m_avio_context { nullptr };
};

}
