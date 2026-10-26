/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RTCEncodedAudioFrame.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>
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

// https://w3c.github.io/webrtc-encoded-transform/#RTCEncodedAudioFrame-serialization
WebIDL::ExceptionOr<void> RTCEncodedAudioFrame::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    // 1. If forStorage is true, then throw a DataCloneError.
    if (for_storage)
        return WebIDL::DataCloneError::create("RTCEncodedAudioFrame is not storable"_utf16);

    // 2. Set serialized.[[metadata]] to an internal representation of value's metadata.
    auto encode_optional = [&serialized]<typename T>(Optional<T> const& v) {
        serialized.encode<bool>(v.has_value());
        if (v.has_value())
            serialized.encode<T>(v.value());
    };
    encode_optional(m_metadata.synchronization_source);
    encode_optional(m_metadata.payload_type);
    serialized.encode<bool>(m_metadata.contributing_sources.has_value());
    if (m_metadata.contributing_sources.has_value()) {
        auto const& csrcs = m_metadata.contributing_sources.value();
        serialized.encode<u64>(csrcs.size());
        for (auto csrc : csrcs)
            serialized.encode<u32>(csrc);
    }
    encode_optional(m_metadata.rtp_timestamp);
    encode_optional(m_metadata.receive_time);
    encode_optional(m_metadata.capture_time);
    encode_optional(m_metadata.sender_capture_time_offset);
    encode_optional(m_metadata.mime_type);
    // sequenceNumber is a WebIDL short; encode widened to int (i16 has no explicit
    // StructuredSerializeWriter instantiation).
    serialized.encode<bool>(m_metadata.sequence_number.has_value());
    if (m_metadata.sequence_number.has_value())
        serialized.encode<int>(m_metadata.sequence_number.value());
    encode_optional(m_metadata.audio_level);

    // 3. Set serialized.[[data]] to the sub-serialization of value.[[data]].
    VERIFY(m_data);
    TRY(HTML::structured_serialize_internal(m_data->vm(), serialized, JS::Value(m_data.ptr()), for_storage, memory));
    return {};
}

WebIDL::ExceptionOr<void> RTCEncodedAudioFrame::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory& memory)
{
    // 1. Set value's metadata to the platform object representation of serialized.[[metadata]].
    auto decode_optional = [&serialized, &realm]<typename T>(Optional<T>& target) -> WebIDL::ExceptionOr<void> {
        if (TRY(HTML::decode_or_throw_data_clone_error<bool>(realm, serialized)))
            target = TRY(HTML::decode_or_throw_data_clone_error<T>(realm, serialized));
        else
            target = {};
        return {};
    };
    TRY(decode_optional(m_metadata.synchronization_source));
    TRY(decode_optional(m_metadata.payload_type));
    if (TRY(HTML::decode_or_throw_data_clone_error<bool>(realm, serialized))) {
        Vector<u32> csrcs;
        auto count = TRY(HTML::decode_or_throw_data_clone_error<u64>(realm, serialized));
        csrcs.ensure_capacity(count);
        for (u64 i = 0; i < count; ++i)
            csrcs.append(TRY(HTML::decode_or_throw_data_clone_error<u32>(realm, serialized)));
        m_metadata.contributing_sources = move(csrcs);
    } else {
        m_metadata.contributing_sources = {};
    }
    TRY(decode_optional(m_metadata.rtp_timestamp));
    TRY(decode_optional(m_metadata.receive_time));
    TRY(decode_optional(m_metadata.capture_time));
    TRY(decode_optional(m_metadata.sender_capture_time_offset));
    TRY(decode_optional(m_metadata.mime_type));
    if (TRY(HTML::decode_or_throw_data_clone_error<bool>(realm, serialized)))
        m_metadata.sequence_number = static_cast<i16>(TRY(HTML::decode_or_throw_data_clone_error<int>(realm, serialized)));
    else
        m_metadata.sequence_number = {};
    TRY(decode_optional(m_metadata.audio_level));

    // 2. Set value.[[data]] to the sub-deserialization of serialized.[[data]].
    auto deserialized_data = TRY(HTML::structured_deserialize_internal(realm.vm(), serialized, realm, memory));
    if (!deserialized_data.is_object() || !is<JS::ArrayBuffer>(deserialized_data.as_object()))
        return WebIDL::DataCloneError::create("RTCEncodedAudioFrame.[[data]] did not deserialize to an ArrayBuffer"_utf16);
    m_data = static_cast<JS::ArrayBuffer&>(deserialized_data.as_object());
    return {};
}

}
