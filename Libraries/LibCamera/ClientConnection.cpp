/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCamera/ClientConnection.h>

namespace Camera {

static NeverDestroyed<ClientConnection::TransportFactory> s_transport_factory;
static NeverDestroyed<RefPtr<ClientConnection>> s_connection;

void ClientConnection::set_transport_factory(TransportFactory factory)
{
    *s_transport_factory = move(factory);
}

bool ClientConnection::has_transport_factory()
{
    return !!*s_transport_factory;
}

ErrorOr<NonnullRefPtr<ClientConnection>> ClientConnection::acquire()
{
    if (*s_connection)
        return NonnullRefPtr(**s_connection);
    if (!*s_transport_factory)
        return Error::from_string_literal("No camera server transport factory is installed");

    auto transport = TRY((*s_transport_factory)());
    auto connection = adopt_ref(*new ClientConnection(move(transport)));
    *s_connection = connection;
    return connection;
}

size_t ClientConnection::capture_stream_count_for_testing()
{
    auto const& connection = *s_connection;
    if (!connection)
        return 0;
    return connection->m_streams.size() + connection->m_pending_streams.size();
}

ClientConnection::ClientConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CameraClientEndpoint, CameraServerEndpoint>(*this, move(transport))
{
}

ClientConnection::~ClientConnection() = default;

void ClientConnection::die()
{
    // NB: Forgetting this connection below can drop the last reference to it.
    NonnullRefPtr keep_alive_until_death_is_handled = *this;
    m_is_dead = true;
    if (*s_connection == this)
        *s_connection = nullptr;

    for (auto& [stream_id, pending] : exchange(m_pending_streams, {}))
        pending.promise->reject(Error::from_string_literal("Camera server connection died before the stream was created"));

    // A stream's owner may let go of it, or of others, as it hears.
    Vector<NonnullRefPtr<CaptureStream>> live_streams;
    for (auto& [stream_id, stream] : exchange(m_streams, {}))
        live_streams.append(*stream);
    for (auto& stream : live_streams)
        stream->connection_lost();

    // Whatever the server knew went with it; the watcher hears so once.
    if (auto on_device_list = move(m_on_device_list))
        on_device_list(Error::from_string_literal("Camera server connection died"));
}

void ClientConnection::watch_devices(DeviceListCallback on_device_list)
{
    VERIFY(!m_on_device_list);
    if (m_is_dead) {
        on_device_list(Error::from_string_literal("Camera server connection has died"));
        return;
    }
    m_on_device_list = move(on_device_list);
    async_watch_devices();
}

void ClientConnection::request_device_list()
{
    if (!m_is_dead)
        async_watch_devices();
}

void ClientConnection::devices_changed(Vector<DeviceInfo> devices)
{
    if (m_on_device_list)
        m_on_device_list(move(devices));
}

NonnullRefPtr<CaptureStream::CreatePromise> ClientConnection::create_capture_stream(StringView device_id, u64 page_label, StreamRequest request, CaptureStream::FrameCallback callback)
{
    auto promise = CaptureStream::CreatePromise::construct();
    if (m_is_dead) {
        promise->reject(Error::from_string_literal("Camera server connection has died"));
        return promise;
    }

    auto stream_id = m_next_stream_id++;
    m_pending_streams.set(stream_id, PendingStream { .promise = promise, .callback = move(callback) });
    async_create_capture_stream(stream_id, device_id, page_label, request);
    return promise;
}

void ClientConnection::capture_stream_destroyed(Badge<CaptureStream>, u64 stream_id)
{
    m_streams.remove(stream_id);
    if (!m_is_dead)
        async_destroy_capture_stream(stream_id);
}

void ClientConnection::capture_stream_created(u64 stream_id, u32 width, u32 height, u32 frame_rate, Vector<Core::AnonymousBuffer> buffers)
{
    auto pending = m_pending_streams.take(stream_id);
    if (!pending.has_value()) {
        async_destroy_capture_stream(stream_id);
        return;
    }

    // Validate buffers once; every frame uses this size.
    auto is_usable = width > 0 && height > 0 && width <= MAXIMUM_PICTURE_DIMENSION && height <= MAXIMUM_PICTURE_DIMENSION && !buffers.is_empty();
    for (auto const& buffer : buffers)
        is_usable = is_usable && buffer.is_valid() && buffer.size() >= i420_size(width, height);
    if (!is_usable) {
        async_destroy_capture_stream(stream_id);
        pending->promise->reject(Error::from_errno(EIO));
        return;
    }

    auto stream = adopt_ref(*new CaptureStream(*this, stream_id, width, height, frame_rate, move(buffers), move(pending->callback)));
    m_streams.set(stream_id, stream.ptr());
    pending->promise->resolve(move(stream));
}

void ClientConnection::capture_stream_creation_failed(u64 stream_id, CaptureFailure failure)
{
    auto pending = m_pending_streams.take(stream_id);
    if (!pending.has_value())
        return;
    pending->promise->reject(Error::from_errno(failure == CaptureFailure::Refused ? EPERM : EIO));
}

void ClientConnection::end_stream(u64 stream_id)
{
    if (auto pending = m_pending_streams.take(stream_id); pending.has_value()) {
        pending->promise->reject(Error::from_errno(EIO));
        return;
    }
    auto stream = m_streams.take(stream_id);
    if (!stream.has_value())
        return;
    NonnullRefPtr<CaptureStream> protector = *stream.value();
    protector->capture_ended();
}

void ClientConnection::capture_stream_closed(u64 stream_id)
{
    // The server closes the streams of a tab whose permission was taken away, which refuses the ones still waiting.
    if (auto pending = m_pending_streams.take(stream_id); pending.has_value()) {
        pending->promise->reject(Error::from_errno(EPERM));
        return;
    }
    end_stream(stream_id);
}

void ClientConnection::capture_stream_ended(u64 stream_id)
{
    end_stream(stream_id);
}

void ClientConnection::frame_captured(u64 stream_id, u32 buffer_index, i64 timestamp_in_microseconds)
{
    // The stream may have gone while the picture was on its way; the server forgot its buffers when it heard.
    auto stream = m_streams.get(stream_id);
    if (!stream.has_value())
        return;
    NonnullRefPtr<CaptureStream> protector = *stream.value();
    if (buffer_index < protector->m_buffers.size())
        protector->deliver(buffer_index, AK::Duration::from_microseconds(timestamp_in_microseconds));
    // The stream's owner may have let go of it on seeing the picture.
    if (m_streams.contains(stream_id))
        async_release_frame(stream_id, buffer_index);
}

}
