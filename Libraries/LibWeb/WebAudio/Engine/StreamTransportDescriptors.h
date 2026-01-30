/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <AudioServer/AudioInputDeviceInfo.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/File.h>
#include <LibWeb/WebAudio/Engine/StreamTransport.h>

namespace Web::WebAudio::Render {

using StreamID = u64;

// Descriptors are control-plane objects: they are passed across process boundaries (IPC)
// to describe how to access the shared-memory data-plane for a stream.
//
// The descriptor does not define lifecycle or ownership policy; those are owned by the session/backend.

struct RingStreamFormat {
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    u32 channel_capacity { 0 };

    // Power-of-two is recommended. The header supports any nonzero value.
    u64 capacity_frames { 0 };
};

struct RingStreamDescriptor {
    StreamID stream_id { 0 };

    RingStreamFormat format;
    StreamOverflowPolicy overflow_policy { StreamOverflowPolicy::DropOldest };

    // Shared memory containing RingStreamHeader followed by interleaved f32 ring data.
    Core::AnonymousBuffer shared_memory;

    // Notification handle for wakeups (eventfd or pipe read end). May be invalid.
    IPC::File notify_fd;
};

struct AudioInputStreamMetadata {
    AudioServer::AudioInputDeviceID device_id { 0 };
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    u64 capacity_frames { 0 };
    u8 overflow_policy { 0 };
};

// Control-plane binding from an opaque render-graph provider_id to a concrete stream descriptor.
// The provider_id is the one carried in the render graph wire format.
struct MediaElementAudioSourceStreamDescriptor {
    u64 provider_id { 0 };
    RingStreamDescriptor ring_stream;
};

struct MediaStreamAudioSourceStreamDescriptor {
    u64 provider_id { 0 };
    AudioInputStreamMetadata metadata;
};

struct PacketStreamDescriptor {
    StreamID stream_id { 0 };

    // Packet streams are currently expressed using Core::SharedBufferStream descriptors at call sites.
    // This wrapper exists to align naming and lifecycle with RingStream.
    IPC::File notify_fd;

    // Future: shared buffer stream control structure.
};

// Descriptor for a Core::SharedBufferStream transport.
// The stream is represented by three shared-memory buffers:
// - pool_buffer: fixed-size block pool
// - ready_ring_buffer: SPSC ring of ready descriptors
// - free_ring_buffer: SPSC ring of free descriptors
struct SharedBufferStreamDescriptor {
    Core::AnonymousBuffer pool_buffer;
    Core::AnonymousBuffer ready_ring_buffer;
    Core::AnonymousBuffer free_ring_buffer;
};

// Control-plane binding for ScriptProcessorNode remote processing.
// WebContent allocates the shared-memory streams and sends them to AudioServer.
//
// request_stream: AudioServer (producer) -> WebContent (consumer)
// response_stream: WebContent (producer) -> AudioServer (consumer)
//
// request_notify_write_fd is the write end used by AudioServer to wake WebContent.
// WebContent keeps the corresponding read end locally.
struct ScriptProcessorStreamDescriptor {
    u64 node_id { 0 };
    u32 buffer_size { 0 };
    u32 input_channel_count { 0 };
    u32 output_channel_count { 0 };

    SharedBufferStreamDescriptor request_stream;
    SharedBufferStreamDescriptor response_stream;

    IPC::File request_notify_write_fd;
};

// Control-plane binding for AudioWorkletNode.port message transport.
// WebContent creates a socketpair-backed MessagePort transport; it keeps one end attached to
// the node-side MessagePort and sends the peer fd to AudioServer so the processor-side
// MessagePort can be attached in the worklet VM.
struct WorkletNodePortDescriptor {
    u64 node_id { 0 };
    IPC::File processor_port_fd;
};

}
