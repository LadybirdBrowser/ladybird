/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibMedia/VideoEdgeQueue.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::VideoEdgeQueue const& edge)
{
    TRY(encoder.encode(TRY(File::clone_fd(edge.ring_fd()))));
    TRY(encoder.encode(edge.header_buffer()));
    return {};
}

template<>
ErrorOr<Media::VideoEdgeQueue> decode(Decoder& decoder)
{
    auto ring_fd = TRY(decoder.decode<File>());
    auto header_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return Media::VideoEdgeQueue::create(ring_fd.take_fd(), move(header_buffer));
}

}
