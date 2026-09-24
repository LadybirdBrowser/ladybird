/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaClient/Client.h>
#include <LibMediaClient/RemoteMediaStream.h>

namespace MediaClient {

NonnullRefPtr<RemoteMediaStream> RemoteMediaStream::create()
{
    auto client_or_error = Client::acquire();
    if (client_or_error.is_error()) {
        dbgln("MediaClient: Could not reach the media server for a media stream: {}", client_or_error.error());
        return adopt_ref(*new RemoteMediaStream(nullptr, 0));
    }
    auto client = client_or_error.release_value();
    auto stream = adopt_ref(*new RemoteMediaStream(client, client->allocate_id()));
    client->register_media_stream({}, *stream);
    client->async_create_media_stream(stream->id());
    return stream;
}

RemoteMediaStream::RemoteMediaStream(RefPtr<Client> client, u64 id)
    : m_client(move(client))
    , m_id(id)
{
}

RemoteMediaStream::~RemoteMediaStream()
{
    if (!m_client)
        return;
    m_client->unregister_media_stream({}, *this);
    if (m_client->is_open())
        m_client->async_destroy_media_stream(m_id);
}

void RemoteMediaStream::set_data_request_callback(DataRequestCallback callback)
{
    m_data_request_callback = move(callback);
}

void RemoteMediaStream::add_chunk_at(u64 offset, ReadonlyBytes bytes)
{
    if (!m_client || !m_client->is_open())
        return;
    for (size_t piece_start = 0; piece_start < bytes.size(); piece_start += MAX_CODED_BYTES_PER_MESSAGE) {
        auto piece = bytes.slice(piece_start, min(MAX_CODED_BYTES_PER_MESSAGE, bytes.size() - piece_start));
        m_client->async_add_media_stream_chunk(m_id, offset + piece_start, piece);
    }
}

void RemoteMediaStream::set_expected_size(u64 size)
{
    if (m_client && m_client->is_open())
        m_client->async_set_media_stream_expected_size(m_id, size);
}

void RemoteMediaStream::set_may_idle(bool may_idle)
{
    if (m_client && m_client->is_open())
        m_client->async_set_media_stream_may_idle(m_id, may_idle);
}

void RemoteMediaStream::close()
{
    if (m_client && m_client->is_open())
        m_client->async_close_media_stream(m_id);
}

void RemoteMediaStream::data_requested(Badge<Client>, Optional<u64> offset)
{
    if (m_data_request_callback)
        m_data_request_callback(offset);
}

}
