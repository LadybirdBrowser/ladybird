/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CameraServer/ServerConnection.h>

namespace CameraServer {

ServerConnection::ServerConnection(NonnullOwnPtr<IPC::Transport> transport, int client_id, CameraBackend& backend, CapturePermissionRequest request_capture_permission)
    : IPC::ConnectionFromClient<CameraClientEndpoint, CameraServerEndpoint>(*this, move(transport), client_id)
    , m_backend(backend)
    , m_request_capture_permission(move(request_capture_permission))
{
}

ServerConnection::~ServerConnection()
{
    close_streams();
}

void ServerConnection::die()
{
    close_streams();
    if (on_death)
        on_death();
}

void ServerConnection::close_streams()
{
    for (auto& [stream_id, stream] : exchange(m_streams, {}))
        stream_gone(stream);
}

void ServerConnection::stream_gone(Stream& stream)
{
    // Disable session callbacks before closing the camera.
    if (stream.session)
        stream.session->close();
    stream.camera = nullptr;
    if (stream.may_capture && on_capture_stopped)
        on_capture_stopped(stream.page_label);
}

void ServerConnection::set_capture_permitted(bool permitted)
{
    if (m_capture_permitted == permitted)
        return;
    m_capture_permitted = permitted;
    if (!permitted) {
        for (auto& [stream_id, stream] : m_streams)
            async_capture_stream_closed(stream_id);
        close_streams();
    }
    if (m_client_watches_devices)
        send_device_list();
}

void ServerConnection::set_devices(Vector<Camera::DeviceInfo> const& devices)
{
    m_devices = devices;
    if (m_client_watches_devices)
        send_device_list();
}

void ServerConnection::watch_devices()
{
    m_client_watches_devices = true;
    // A list not yet available follows as soon as the platform reports one.
    if (m_devices.has_value())
        send_device_list();
}

void ServerConnection::send_device_list()
{
    if (!m_devices.has_value())
        return;
    // A page without the permission may learn only that cameras exist, so that is all its process learns.
    if (!m_capture_permitted) {
        Vector<Camera::DeviceInfo> existence;
        if (!m_devices->is_empty())
            existence.append(Camera::DeviceInfo { .dom_device_id = {}, .label = {}, .group_id = {}, .is_default = true });
        async_devices_changed(move(existence));
        return;
    }
    async_devices_changed(*m_devices);
}

void ServerConnection::create_capture_stream(u64 stream_id, ByteString device_id, u64 page_label, Camera::StreamRequest request)
{
    if (m_streams.contains(stream_id)) {
        did_misbehave("create_capture_stream: stream ID already exists");
        return;
    }
    if (m_streams.size() >= MAXIMUM_STREAMS) {
        async_capture_stream_creation_failed(stream_id, Camera::CaptureFailure::DeviceUnavailable);
        return;
    }

    // The Browser rules on every request, so a client once allowed cannot simply start another.
    Stream stream;
    stream.generation = m_next_stream_generation++;
    stream.page_label = page_label;
    auto generation = stream.generation;
    m_streams.set(stream_id, move(stream));
    m_request_capture_permission(device_id, page_label, [weak_self = make_weak_ptr<ServerConnection>(), stream_id, generation, device_id, request](bool allowed) {
        if (auto self = weak_self.strong_ref())
            self->capture_permission_decided(stream_id, generation, device_id, request, allowed);
    });
}

void ServerConnection::capture_permission_decided(u64 stream_id, u64 generation, ByteString const& device_id, Camera::StreamRequest request, bool allowed)
{
    // The client may have given up on the stream, or the tab lost the permission, while the Browser was asked. It may
    // even have asked for another stream under the same ID since, which this decision is not for.
    auto stream = m_streams.get(stream_id);
    if (!stream.has_value() || stream->generation != generation)
        return;
    if (!allowed) {
        m_streams.remove(stream_id);
        async_capture_stream_creation_failed(stream_id, Camera::CaptureFailure::Refused);
        return;
    }

    // The stream counts as capturing from here on, even while the camera opens.
    stream->may_capture = true;
    if (on_capture_started)
        on_capture_started(stream->page_label);

    auto weak_self = make_weak_ptr<ServerConnection>();
    auto fail = [weak_self, stream_id] {
        auto self = weak_self.strong_ref();
        if (!self)
            return;
        auto stream = self->m_streams.take(stream_id);
        if (!stream.has_value())
            return;
        self->stream_gone(*stream);
        if (stream->is_open)
            self->async_capture_stream_ended(stream_id);
        else
            self->async_capture_stream_creation_failed(stream_id, Camera::CaptureFailure::DeviceUnavailable);
    };

    // The client hears of the stream once the camera says what it gives, and of a camera that fails before that as one
    // that would not open.
    auto session = CaptureSession::create({
                                              .on_ready = [weak_self, stream_id](u32 width, u32 height, u32 frame_rate, Vector<Core::AnonymousBuffer> const& buffers) {
            auto self = weak_self.strong_ref();
            if (!self)
                return;
            auto stream = self->m_streams.get(stream_id);
            if (!stream.has_value())
                return;
            stream->is_open = true;
            self->async_capture_stream_created(stream_id, width, height, frame_rate, buffers); },
                                              .on_frame = [weak_self, stream_id](u32 buffer_index, AK::Duration timestamp) {
            auto self = weak_self.strong_ref();
            if (self && self->m_streams.contains(stream_id))
                self->async_frame_captured(stream_id, buffer_index, timestamp.to_microseconds()); },
                                              .on_ended = [fail] { fail(); },
                                          },
        request);
    stream->session = session;

    auto camera = m_backend.open(device_id, request, session);
    if (camera.is_error()) {
        warnln("CameraServer: Unable to open a camera: {}", camera.error());
        fail();
        return;
    }
    stream->camera = camera.release_value();
}

void ServerConnection::destroy_capture_stream(u64 stream_id)
{
    if (auto stream = m_streams.take(stream_id); stream.has_value())
        stream_gone(*stream);
}

void ServerConnection::release_frame(u64 stream_id, u32 buffer_index)
{
    auto stream = m_streams.get(stream_id);
    if (stream.has_value() && stream->session)
        stream->session->release_frame(buffer_index);
}

}
