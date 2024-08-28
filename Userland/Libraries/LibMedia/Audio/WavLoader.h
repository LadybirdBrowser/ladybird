/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Loader.h"
#include <AK/ByteString.h>
#include <AK/FixedArray.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <LibRIFF/RIFF.h>

namespace Audio {

// Loader for the WAVE (file extension .wav) uncompressed audio file format.
// WAVE uses the Microsoft RIFF container.
// Original RIFF Spec, without later extensions: https://www.aelius.com/njh/wavemetatools/doc/riffmci.pdf
// More concise WAVE information plus various spec links: http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
class WavLoaderPlugin : public LoaderPlugin {
public:
    explicit WavLoaderPlugin(NonnullOwnPtr<SeekableStream> stream);

    static bool sniff(SeekableStream& stream);
    static ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> create(NonnullOwnPtr<SeekableStream>);

    virtual ErrorOr<Vector<FixedArray<Sample>>, LoaderError> load_chunks(size_t samples_to_read_from_input) override;

    virtual MaybeLoaderError reset() override { return seek(0); }

    // sample_index 0 is the start of the raw audio sample data
    // within the file/stream.
    virtual MaybeLoaderError seek(int sample_index) override;

    virtual int loaded_samples() override { return static_cast<int>(m_loaded_samples); }
    virtual int total_samples() override { return static_cast<int>(m_total_samples); }
    virtual u32 sample_rate() override { return m_sample_rate; }
    virtual u16 num_channels() override { return m_num_channels; }
    virtual ByteString format_name() override { return "RIFF WAVE (.wav)"; }
    virtual PcmSampleFormat pcm_format() override { return m_sample_format; }

private:
    MaybeLoaderError parse_header();
    MaybeLoaderError load_wav_info_block(Vector<RIFF::OwnedChunk> info_chunks);

    LoaderSamples samples_from_pcm_data(ReadonlyBytes data, size_t samples_to_read) const;
    template<typename SampleReader>
    MaybeLoaderError read_samples_from_stream(Stream& stream, SampleReader read_sample, FixedArray<Sample>& samples) const;

    u32 m_sample_rate { 0 };
    u16 m_num_channels { 0 };
    PcmSampleFormat m_sample_format;
    size_t m_byte_offset_of_data_samples { 0 };

    size_t m_loaded_samples { 0 };
    size_t m_total_samples { 0 };
};

}
