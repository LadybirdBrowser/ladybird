/*
 * Copyright (c) 2026, Naoki Ikeguchi <me@s6n.jp>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioDataPrototype.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebCodecs {

struct AudioDataInit {
    Bindings::AudioSampleFormat format;
    float sample_rate;
    WebIDL::UnsignedLong number_of_frames;
    WebIDL::UnsignedLong number_of_channels;
    WebIDL::LongLong timestamp; // microseconds
    GC::Root<WebIDL::BufferSource> data;
    // Optional<Vector<ByteBuffer>> transfer;
};

struct AudioDataCopyToOptions {
    WebIDL::UnsignedLong plane_index;
    Optional<WebIDL::UnsignedLong> frame_offset;
    Optional<WebIDL::UnsignedLong> frame_count;
    Optional<Bindings::AudioSampleFormat> format;
};

// https://w3c.github.io/webcodecs/#audiodata-interface
class AudioData final : public Bindings::PlatformObject
    , public Bindings::Serializable
    , public Bindings::Transferable {
    WEB_PLATFORM_OBJECT(AudioData, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AudioData);

    static WebIDL::ExceptionOr<GC::Ref<AudioData>> construct_impl(JS::Realm&, AudioDataInit const&);
    void initialize(JS::Realm&) override;

    Optional<Bindings::AudioSampleFormat> format() const { return this->m_format; }
    float sample_rate() const { return this->m_sample_rate; }
    WebIDL::UnsignedLong number_of_frames() const { return this->m_number_of_frames; }
    WebIDL::UnsignedLong number_of_channels() const { return this->m_number_of_channels; }
    WebIDL::UnsignedLongLong duration() const;
    WebIDL::LongLong timestamp() const { return this->m_timestamp; }

    WebIDL::ExceptionOr<WebIDL::UnsignedLong> allocation_size(AudioDataCopyToOptions const&);
    WebIDL::ExceptionOr<void> copy_to(GC::Root<WebIDL::BufferSource> const& destination, AudioDataCopyToOptions const&);
    GC::Ref<AudioData> clone() const;
    void close();

    // ^Web::Bindings::Serializable
    HTML::SerializeType serialize_type() const override { return HTML::SerializeType::AudioData; }
    WebIDL::ExceptionOr<void> serialization_steps(HTML::TransferDataEncoder&, bool, HTML::SerializationMemory&) override;
    WebIDL::ExceptionOr<void> deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

    // ^Web::Bindings::Transferable
    WebIDL::ExceptionOr<void> transfer_steps(HTML::TransferDataEncoder&) override;
    WebIDL::ExceptionOr<void> transfer_receiving_steps(HTML::TransferDataDecoder&) override;
    HTML::TransferType primary_interface() const override;

protected:
    explicit AudioData(JS::Realm& realm);
    ~AudioData() override;

private:
    Optional<ByteBuffer> m_data;
    Optional<Bindings::AudioSampleFormat> m_format;
    float m_sample_rate = 0;
    WebIDL::UnsignedLong m_number_of_frames = 0;
    WebIDL::UnsignedLong m_number_of_channels = 0;
    WebIDL::LongLong m_timestamp = 0;

    WebIDL::ExceptionOr<WebIDL::UnsignedLong> compute_copy_element_count(AudioDataCopyToOptions const&) const;
};

bool is_valid_audio_data_init(AudioDataInit const&);
bool is_format_interleaved(Bindings::AudioSampleFormat);
WebIDL::UnsignedLong get_bytes_per_sample(Bindings::AudioSampleFormat);
Variant<Span<u8>, Span<i16>, Span<i32>, Span<f32>> buffer_as_samples(Span<u8> buf, Bindings::AudioSampleFormat format);

template<typename Source, typename Target>
Target convert_sample_format(Source value);

}
