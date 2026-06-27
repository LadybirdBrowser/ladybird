/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/SeqLock.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Export.h>
#include <LibMedia/VideoFrameHandle.h>

namespace Media {

// A single-slot shared-memory page holding the handle of the frame the hosted sink is currently presenting.
// The presenting side writes it whenever its current frame changes; the producer-owning side reads it
// synchronously to serve canvas/WebGL captures without a round trip. The seqlock makes a recycle race
// fail the read rather than expose a torn handle.
class PresentedFramePage {
public:
    static ErrorOr<PresentedFramePage> create()
    {
        auto buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(SeqLock<VideoFrameHandle>)));
        auto* seqlock = new (buffer.data<void>()) SeqLock<VideoFrameHandle>;
        return PresentedFramePage { move(buffer), seqlock };
    }

    static ErrorOr<PresentedFramePage> create(Core::AnonymousBuffer buffer)
    {
        if (!buffer.is_valid() || buffer.size() < sizeof(SeqLock<VideoFrameHandle>))
            return Error::from_string_literal("Invalid presented frame page buffer");
        auto* seqlock = reinterpret_cast<SeqLock<VideoFrameHandle>*>(buffer.data<void>());
        return PresentedFramePage { move(buffer), seqlock };
    }

    Core::AnonymousBuffer const& buffer() const { return m_buffer; }

    // Presenting side.
    void store(VideoFrameHandle handle) { m_seqlock->store(handle); }

    // Reading side.
    Optional<VideoFrameHandle> read() const { return m_seqlock->read(); }

private:
    PresentedFramePage(Core::AnonymousBuffer buffer, SeqLock<VideoFrameHandle>* seqlock)
        : m_buffer(move(buffer))
        , m_seqlock(seqlock)
    {
    }

    Core::AnonymousBuffer m_buffer;
    SeqLock<VideoFrameHandle>* m_seqlock { nullptr };
};

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, Media::PresentedFramePage const&);

template<>
MEDIA_API ErrorOr<Media::PresentedFramePage> decode(Decoder&);

}
