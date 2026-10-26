/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/RTCEncodedAudioFrame.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebRTC/RTCEncodedAudioFrame.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCEncodedAudioFrame);

GC::Ref<RTCEncodedAudioFrame> RTCEncodedAudioFrame::create()
{
    return GC::Heap::the().allocate<RTCEncodedAudioFrame>();
}

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcencodedaudioframe-rtcencodedaudioframe
WebIDL::ExceptionOr<GC::Ref<RTCEncodedAudioFrame>> RTCEncodedAudioFrame::construct_impl(JS::Realm& realm, GC::Ref<RTCEncodedAudioFrame> original_frame, RTCEncodedAudioFrameOptions const& options)
{
    auto frame = create();

    // 1. Let this.[[data]] be the result of CloneArrayBuffer(originalFrame.[[data]], 0,
    //    originalFrame.[[data]].[[ArrayBufferByteLength]]).
    auto& original_data = *original_frame->m_data;
    auto* cloned = TRY(JS::clone_array_buffer(realm.vm(), original_data, 0, original_data.byte_length()));
    frame->m_data = cloned;

    // 2. Let [[metadata]] represent the metadata associated with this newly constructed frame.
    //    2.1. Copy each field of originalFrame's [[metadata]] into this.[[metadata]].
    frame->m_metadata = original_frame->m_metadata;
    //    2.2. For each field present in options.metadata, override this.[[metadata]] with a deep copy.
    if (options.metadata.has_value()) {
        auto const& source = options.metadata.value();
        if (source.synchronization_source.has_value())
            frame->m_metadata.synchronization_source = source.synchronization_source;
        if (source.payload_type.has_value())
            frame->m_metadata.payload_type = source.payload_type;
        if (source.contributing_sources.has_value())
            frame->m_metadata.contributing_sources = source.contributing_sources;
        if (source.rtp_timestamp.has_value())
            frame->m_metadata.rtp_timestamp = source.rtp_timestamp;
        if (source.receive_time.has_value())
            frame->m_metadata.receive_time = source.receive_time;
        if (source.capture_time.has_value())
            frame->m_metadata.capture_time = source.capture_time;
        if (source.sender_capture_time_offset.has_value())
            frame->m_metadata.sender_capture_time_offset = source.sender_capture_time_offset;
        if (source.mime_type.has_value())
            frame->m_metadata.mime_type = source.mime_type;
        if (source.sequence_number.has_value())
            frame->m_metadata.sequence_number = source.sequence_number;
        if (source.audio_level.has_value())
            frame->m_metadata.audio_level = source.audio_level;
    }
    return frame;
}

GC::Ref<RTCEncodedAudioFrame> RTCEncodedAudioFrame::create_from_packet(JS::Realm& realm, ByteBuffer payload,
    u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number)
{
    auto frame = create();
    frame->m_data = JS::ArrayBuffer::create(realm, move(payload));
    frame->m_metadata.synchronization_source = ssrc;
    frame->m_metadata.payload_type = payload_type;
    frame->m_metadata.rtp_timestamp = rtp_timestamp;
    // sequenceNumber is signed 16-bit per spec — RTP seq is u16, just bit-cast.
    frame->m_metadata.sequence_number = static_cast<i16>(sequence_number);
    frame->m_metadata.mime_type = "audio/opus"_utf16;
    return frame;
}

RTCEncodedAudioFrame::RTCEncodedAudioFrame() = default;

RTCEncodedAudioFrame::~RTCEncodedAudioFrame() = default;

void RTCEncodedAudioFrame::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
}

GC::Ref<JS::ArrayBuffer> RTCEncodedAudioFrame::data() const
{
    return *m_data;
}

WebIDL::ExceptionOr<void> RTCEncodedAudioFrame::set_data(GC::Ref<JS::ArrayBuffer> buffer)
{
    m_data = buffer;
    return {};
}

GC::Ref<RTCEncodedAudioFrame> RTCEncodedAudioFrame::create(GC::Ref<JS::ArrayBuffer> data, RTCEncodedAudioFrameMetadata metadata)
{
    auto frame = create();
    frame->m_data = data;
    frame->m_metadata = move(metadata);
    return frame;
}

// The Rust serialization steps borrow the spans and take ownership of the string reference.
// Deserialization lends both to C++ for the duration of the call.
template<typename T>
struct OptionalFrameField {
    bool present { false };
    T value {};
};

struct RTCEncodedAudioFrameFields {
    OptionalFrameField<u32> synchronization_source;
    OptionalFrameField<u8> payload_type;
    OptionalFrameField<ReadonlySpan<u32>> contributing_sources;
    OptionalFrameField<u32> rtp_timestamp;
    OptionalFrameField<double> receive_time;
    OptionalFrameField<double> capture_time;
    OptionalFrameField<double> sender_capture_time_offset;
    OptionalFrameField<FlatPtr> mime_type;
    OptionalFrameField<i16> sequence_number;
    OptionalFrameField<double> audio_level;
};

extern "C" {
JSObject* web_rtc_encoded_audio_frame_with_fields(JSObject*, void* state, void (*steps)(void*, RTCEncodedAudioFrameFields const*));
JSObject* web_rtc_encoded_audio_frame_create(void* realm, RTCEncodedAudioFrameFields const*, JSObject* data);
}

JSObject* web_rtc_encoded_audio_frame_with_fields(JSObject* object, void* state, void (*steps)(void*, RTCEncodedAudioFrameFields const*))
{
    auto const& frame = Bindings::impl_from<RTCEncodedAudioFrame>(*JS::HostABI::object_from_abi(object));
    auto metadata = frame.get_metadata();
    RTCEncodedAudioFrameFields fields;
    if (metadata.synchronization_source.has_value())
        fields.synchronization_source = { true, metadata.synchronization_source.value() };
    if (metadata.payload_type.has_value())
        fields.payload_type = { true, metadata.payload_type.value() };
    if (metadata.contributing_sources.has_value())
        fields.contributing_sources = { true, metadata.contributing_sources.value().span() };
    if (metadata.rtp_timestamp.has_value())
        fields.rtp_timestamp = { true, metadata.rtp_timestamp.value() };
    if (metadata.receive_time.has_value())
        fields.receive_time = { true, metadata.receive_time.value() };
    if (metadata.capture_time.has_value())
        fields.capture_time = { true, metadata.capture_time.value() };
    if (metadata.sender_capture_time_offset.has_value())
        fields.sender_capture_time_offset = { true, metadata.sender_capture_time_offset.value() };
    if (metadata.mime_type.has_value())
        fields.mime_type = { true, metadata.mime_type.value().to_raw_leaked() };
    if (metadata.sequence_number.has_value())
        fields.sequence_number = { true, metadata.sequence_number.value() };
    if (metadata.audio_level.has_value())
        fields.audio_level = { true, metadata.audio_level.value() };
    steps(state, &fields);
    return JS::HostABI::object_to_abi(frame.data().ptr());
}

JSObject* web_rtc_encoded_audio_frame_create(void* realm, RTCEncodedAudioFrameFields const* fields, JSObject* data)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    RTCEncodedAudioFrameMetadata metadata;
    if (fields->synchronization_source.present)
        metadata.synchronization_source = fields->synchronization_source.value;
    if (fields->payload_type.present)
        metadata.payload_type = fields->payload_type.value;
    if (fields->contributing_sources.present)
        metadata.contributing_sources = Vector<u32> { fields->contributing_sources.value };
    if (fields->rtp_timestamp.present)
        metadata.rtp_timestamp = fields->rtp_timestamp.value;
    if (fields->receive_time.present)
        metadata.receive_time = fields->receive_time.value;
    if (fields->capture_time.present)
        metadata.capture_time = fields->capture_time.value;
    if (fields->sender_capture_time_offset.present)
        metadata.sender_capture_time_offset = fields->sender_capture_time_offset.value;
    if (fields->mime_type.present)
        metadata.mime_type = Utf16String::from_raw(fields->mime_type.value);
    if (fields->sequence_number.present)
        metadata.sequence_number = fields->sequence_number.value;
    if (fields->audio_level.present)
        metadata.audio_level = fields->audio_level.value;
    auto frame = RTCEncodedAudioFrame::create(as<JS::ArrayBuffer>(*JS::HostABI::object_from_abi(data)), move(metadata));
    return JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, frame).ptr());
}

}
