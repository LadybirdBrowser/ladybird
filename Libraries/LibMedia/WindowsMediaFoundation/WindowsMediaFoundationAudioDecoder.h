/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibAudio/SampleSpecification.h>
#include <LibMedia/AudioDiscardIntervals.h>
#include <LibMedia/Export.h>
#include <LibMediaCodecs/AudioDecoder.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>

struct IMFSample;

namespace Media::WindowsMediaFoundation {

// Decodes the audio codecs we cannot ship decoders for through the ones Windows provides as Media Foundation
// transforms.
class MEDIA_API WindowsMediaFoundationAudioDecoder final : public AudioDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<WindowsMediaFoundationAudioDecoder>> try_create(CodecID, Audio::SampleSpecification const&, ReadonlyBytes codec_initialization_data);

    virtual ~WindowsMediaFoundationAudioDecoder() override;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<void> write_next_block(AudioBlock&) override;

    virtual void flush() override;

    // Holds everything that names a Media Foundation type, so that the Windows headers stay out of this one.
    struct Transform;

private:
    WindowsMediaFoundationAudioDecoder(CodecID, Audio::SampleSpecification const&, FixedArray<u8>&& codec_configuration);

    DecoderErrorOr<void> ensure_transform_for_frame(CodedFrame const&);
    DecoderErrorOr<void> copy_output_sample(Transform&, IMFSample&);
    void write_output_into_block(AudioBlock&);
    void switch_to_replacement_transform();

    CodecID const m_codec_id;
    Audio::SampleSpecification const m_container_sample_specification;
    FixedArray<u8> m_codec_configuration;

    OwnPtr<Transform> m_transform;
    // Takes over once the transform it replaces has drained what it withheld.
    OwnPtr<Transform> m_replacement_transform;
    bool m_end_of_stream_signaled { false };

    // Each output sample is timed from the packet it was decoded from, and its frames are counted from there.
    Optional<AK::Duration> m_output_timeline_start;
    i64 m_frames_output_on_timeline { 0 };

    // The interleaved samples of the output being written out.
    Vector<float> m_output_samples;
    Audio::SampleSpecification m_output_sample_specification;
    size_t m_output_frames_written { 0 };

    AudioDiscardIntervals m_discard_intervals;
};

}
