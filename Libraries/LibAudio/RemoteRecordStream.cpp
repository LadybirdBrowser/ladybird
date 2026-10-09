/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/ClientConnection.h>
#include <LibAudio/RemoteRecordStream.h>
#include <LibCore/EventLoop.h>

namespace Audio {

RemoteRecordStream::RemoteRecordStream(ClientConnection& connection, u64 stream_id, SampleSpecification specification, RecordCallback callback)
    : m_connection(connection)
    , m_control_loop(connection.event_loop())
    , m_stream_id(stream_id)
    , m_sample_specification(specification)
    , m_callback(move(callback))
{
    m_connection->register_record_stream({}, *this);
}

RemoteRecordStream::~RemoteRecordStream()
{
    m_connection->unregister_record_stream({}, *this);
    // The last reference may go on the pump thread. The connection is reference counted on its own thread only, so
    // the reference moves into the deferred call and is released there, along with the goodbye.
    auto say_goodbye = [connection = move(m_connection), stream_id = m_stream_id] {
        connection->record_stream_unsubscribed({}, stream_id);
    };
    if (Core::EventLoop::is_running() && &Core::EventLoop::current() == &m_control_loop) {
        say_goodbye();
        return;
    }
    m_control_loop.deferred_invoke(move(say_goodbye));
}

void RemoteRecordStream::deliver(ReadonlySpan<float> interleaved_samples)
{
    m_callback(ReadonlyBytes { reinterpret_cast<u8 const*>(interleaved_samples.data()), interleaved_samples.size() * sizeof(float) }, m_sample_specification);
}

void RemoteRecordStream::connection_lost()
{
    if (on_capture_lost)
        on_capture_lost();
}

}
