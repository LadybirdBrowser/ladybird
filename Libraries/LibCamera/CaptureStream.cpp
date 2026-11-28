/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCamera/CaptureStream.h>
#include <LibCamera/ClientConnection.h>

namespace Camera {

NonnullRefPtr<CaptureStream::CreatePromise> CaptureStream::create(StringView device_id, u64 page_label, StreamRequest request, FrameCallback callback)
{
    auto connection = ClientConnection::acquire();
    if (connection.is_error())
        return CreatePromise::rejected(connection.release_error());
    return connection.value()->create_capture_stream(device_id, page_label, request, move(callback));
}

CaptureStream::CaptureStream(ClientConnection& connection, u64 stream_id, u32 width, u32 height, u32 frame_rate, Vector<Core::AnonymousBuffer> buffers, FrameCallback callback)
    : m_connection(connection)
    , m_stream_id(stream_id)
    , m_width(width)
    , m_height(height)
    , m_frame_rate(frame_rate)
    , m_buffers(move(buffers))
    , m_callback(move(callback))
{
}

CaptureStream::~CaptureStream()
{
    if (m_connection)
        m_connection->capture_stream_destroyed({}, m_stream_id);
}

void CaptureStream::deliver(u32 buffer_index, AK::Duration timestamp)
{
    // The connection checked the index, and the buffers' sizes when the stream was made.
    auto const& buffer = m_buffers[buffer_index];
    m_callback(Frame {
        .i420 = { buffer.data<u8>(), i420_size(m_width, m_height) },
        .width = m_width,
        .height = m_height,
        .timestamp = timestamp,
    });
}

void CaptureStream::connection_lost()
{
    m_connection = nullptr;
    if (on_capture_lost)
        on_capture_lost();
}

void CaptureStream::capture_ended()
{
    m_connection = nullptr;
    if (on_capture_ended)
        on_capture_ended();
}

}
