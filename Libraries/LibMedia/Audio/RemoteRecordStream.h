/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/RecordStream.h>
#include <LibMedia/Export.h>

namespace Audio {

class ClientConnection;

// A RecordStream whose device lives in the AudioServer. The server fills one ring per device per client process with
// the device's frames; the connection's pump thread pops them and hands them to every stream on that device.
class MEDIA_API RemoteRecordStream final : public RecordStream {
public:
    // Control thread, once the server has handed over the ring for the stream's device.
    RemoteRecordStream(ClientConnection&, u64 stream_id, SampleSpecification, RecordCallback);
    virtual ~RemoteRecordStream() override;

    virtual SampleSpecification const& sample_specification() const override { return m_sample_specification; }
    u64 stream_id() const { return m_stream_id; }

    // Pump thread. Frames the connection popped from the device's ring, in this stream's format.
    void deliver(ReadonlySpan<float> interleaved_samples);
    // Control thread. The connection died: nothing more arrives, and the owner should open a new stream.
    void connection_lost();

private:
    // Only ever referenced and released on the control thread; see the destructor.
    NonnullRefPtr<ClientConnection> m_connection;
    Core::EventLoop& m_control_loop;
    u64 const m_stream_id { 0 };
    SampleSpecification const m_sample_specification;
    RecordCallback m_callback;
};

}
