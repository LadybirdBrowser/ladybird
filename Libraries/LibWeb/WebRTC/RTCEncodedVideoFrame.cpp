/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RTCEncodedVideoFrame.h>
#include <LibWeb/WebRTC/RTCEncodedVideoFrame.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCEncodedVideoFrame);

GC::Ref<RTCEncodedVideoFrame> RTCEncodedVideoFrame::create()
{
    return GC::Heap::the().allocate<RTCEncodedVideoFrame>();
}

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcencodedvideoframe-constructor
WebIDL::ExceptionOr<GC::Ref<RTCEncodedVideoFrame>> RTCEncodedVideoFrame::construct_impl(JS::Realm& realm, GC::Ref<RTCEncodedVideoFrame> original_frame, RTCEncodedVideoFrameOptions const& options)
{
    auto frame = create();

    // 1. Set this.[[data]] to the result of CloneArrayBuffer(originalFrame.[[data]], 0,
    //    originalFrame.[[data]].[[ArrayBufferByteLength]]).
    auto& original_data = *original_frame->m_data;
    // AD-HOC: CloneArrayBuffer is given a buffer that isn't detached, which script can do to a frame's data.
    if (original_data.is_detached())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "The frame's data is detached"_utf16 };
    frame->m_data = TRY(JS::clone_array_buffer(realm.vm(), original_data, 0, original_data.byte_length()));

    // 2. Let [[metadata]] represent the metadata associated with this newly constructed frame.
    //    1. For each {[[key]],[[value]]} pair of originalFrame.[[metadata]], set this.[[metadata]].[[key]] to a
    //       deep copy of [[value]].
    frame->m_metadata = original_frame->m_metadata;
    frame->m_video = original_frame->m_video;

    //    2. If options.metadata is present, for each {[[key]],[[value]]} pair in options.metadata, set
    //       this.[[metadata]].[[key]] to a deep copy of [[value]].
    if (options.metadata.has_value()) {
        auto const& source = options.metadata.value();
        auto& metadata = frame->m_metadata;
        if (source.synchronization_source.has_value())
            metadata.synchronization_source = source.synchronization_source;
        if (source.payload_type.has_value())
            metadata.payload_type = source.payload_type;
        if (source.contributing_sources.has_value())
            metadata.contributing_sources = source.contributing_sources;
        if (source.rtp_timestamp.has_value())
            metadata.rtp_timestamp = source.rtp_timestamp;
        if (source.receive_time.has_value())
            metadata.receive_time = source.receive_time;
        if (source.capture_time.has_value())
            metadata.capture_time = source.capture_time;
        if (source.sender_capture_time_offset.has_value())
            metadata.sender_capture_time_offset = source.sender_capture_time_offset;
        if (source.mime_type.has_value())
            metadata.mime_type = source.mime_type;
        if (source.frame_id.has_value())
            metadata.frame_id = source.frame_id;
        if (source.dependencies.has_value())
            metadata.dependencies = source.dependencies;
        if (source.width.has_value())
            metadata.width = source.width;
        if (source.height.has_value())
            metadata.height = source.height;
        if (source.spatial_index.has_value())
            metadata.spatial_index = source.spatial_index;
        if (source.temporal_index.has_value())
            metadata.temporal_index = source.temporal_index;
        if (source.timestamp.has_value())
            metadata.timestamp = source.timestamp;
    }
    return frame;
}

static Utf16String mime_type_of(Media::CodecID codec)
{
    switch (codec) {
    case Media::CodecID::VP8:
        return "video/VP8"_utf16;
    case Media::CodecID::VP9:
        return "video/VP9"_utf16;
    case Media::CodecID::H264:
        return "video/H264"_utf16;
    default:
        VERIFY_NOT_REACHED();
    }
}

GC::Ref<RTCEncodedVideoFrame> RTCEncodedVideoFrame::create_from_encoded_frame(JS::Realm& realm, EncodedFrame encoded_frame)
{
    auto frame = create();
    frame->m_video = encoded_frame.video.release_value();
    frame->m_data = JS::ArrayBuffer::create(realm, move(encoded_frame.payload));
    frame->m_metadata.synchronization_source = encoded_frame.ssrc;
    frame->m_metadata.payload_type = encoded_frame.payload_type;
    frame->m_metadata.rtp_timestamp = encoded_frame.rtp_timestamp;
    frame->m_metadata.mime_type = mime_type_of(frame->m_video.codec);
    // https://w3c.github.io/webrtc-encoded-transform/#dom-rtcencodedvideoframemetadata-frameid
    // Only present for received frames if the Dependency Descriptor Header Extension is present.
    // NB: The backend negotiates no Dependency Descriptor, so the frames' own count is kept for sent frames alone.
    if (frame->m_video.is_outgoing())
        frame->m_metadata.frame_id = frame->m_video.frame_id;
    return frame;
}

ErrorOr<EncodedFrame> RTCEncodedVideoFrame::to_encoded_frame() const
{
    return EncodedFrame {
        .payload = TRY(m_data->copy_to_byte_buffer(0, m_data->byte_length())),
        .ssrc = m_metadata.synchronization_source.value_or(0),
        .payload_type = m_metadata.payload_type.value_or(0),
        .rtp_timestamp = m_metadata.rtp_timestamp.value_or(0),
        .sequence_number = 0,
        .video = m_video,
    };
}

RTCEncodedVideoFrame::RTCEncodedVideoFrame() = default;
RTCEncodedVideoFrame::~RTCEncodedVideoFrame() = default;

void RTCEncodedVideoFrame::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
}

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcencodedvideoframe-type
Bindings::EncodedVideoChunkType RTCEncodedVideoFrame::type() const
{
    return m_video.is_key_frame ? Bindings::EncodedVideoChunkType::Key : Bindings::EncodedVideoChunkType::Delta;
}

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcencodedvideoframe-data
GC::Ref<JS::ArrayBuffer> RTCEncodedVideoFrame::data() const
{
    return *m_data;
}

WebIDL::ExceptionOr<void> RTCEncodedVideoFrame::set_data(GC::Ref<JS::ArrayBuffer> buffer)
{
    m_data = buffer;
    return {};
}

}
