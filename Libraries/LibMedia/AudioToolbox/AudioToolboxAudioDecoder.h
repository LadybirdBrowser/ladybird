/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioDecoder.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderCapabilities.h>
#include <LibMedia/Export.h>

namespace Media::AudioToolbox {

class MEDIA_API AudioToolboxAudioDecoder final : public AudioDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<AudioToolboxAudioDecoder>> try_create(CodecID, Audio::SampleSpecification const&, ReadonlyBytes codec_initialization_data);

    virtual ~AudioToolboxAudioDecoder() override;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<void> write_next_block(AudioBlock&) override;

    virtual void flush() override;

private:
    struct Converter;

    static DecoderErrorOr<NonnullOwnPtr<Converter>> create_converter(CodecID, Audio::SampleSpecification const& container_sample_specification, ReadonlyBytes codec_configuration);

    AudioToolboxAudioDecoder(CodecID, Audio::SampleSpecification const&, FixedArray<u8>&& codec_configuration);

    DecoderErrorOr<void> ensure_converter_for_frame(CodedFrame const&);
    void start_output_timeline_at_pending_packet();
    DecoderErrorOr<void> convert_into_block(AudioBlock&);

    CodecID const m_codec_id;
    Audio::SampleSpecification const m_container_sample_specification;
    FixedArray<u8> m_codec_configuration;

    OwnPtr<Converter> m_converter;
    // Takes over once the converter it replaces has output what it withheld.
    OwnPtr<Converter> m_replacement_converter;

    ByteBuffer m_pending_packet;
    AK::Duration m_pending_packet_timestamp;
    bool m_has_pending_packet { false };
    bool m_converter_awaits_input { true };
    bool m_end_of_stream_signaled { false };

    // The converter's output carries no timestamps, so they are counted from the packet that started the timeline.
    AK::Duration m_output_timeline_start;
    i64 m_frames_output_on_timeline { 0 };
};

}
