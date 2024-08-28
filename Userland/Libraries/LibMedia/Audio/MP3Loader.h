/*
 * Copyright (c) 2021, Arne Elster <arne@elster.li>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Loader.h"
#include "MDCT.h"
#include "MP3Types.h"
#include <AK/BitStream.h>
#include <AK/MemoryStream.h>

namespace Audio {

namespace MP3::Tables {
struct ScaleFactorBand;
}

class MP3LoaderPlugin : public LoaderPlugin {
public:
    explicit MP3LoaderPlugin(NonnullOwnPtr<SeekableStream> stream);
    virtual ~MP3LoaderPlugin() = default;

    static bool sniff(SeekableStream& stream);
    static ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> create(NonnullOwnPtr<SeekableStream>);

    virtual ErrorOr<Vector<FixedArray<Sample>>, LoaderError> load_chunks(size_t samples_to_read_from_input) override;

    virtual MaybeLoaderError reset() override;
    virtual MaybeLoaderError seek(int const position) override;

    virtual int loaded_samples() override { return m_loaded_samples; }
    virtual int total_samples() override { return m_total_samples; }
    virtual u32 sample_rate() override { return m_sample_rate; }
    virtual u16 num_channels() override { return m_num_channels; }
    virtual PcmSampleFormat pcm_format() override { return m_sample_format; }
    virtual ByteString format_name() override { return "MP3 (.mp3)"; }

private:
    MaybeLoaderError initialize();
    static MaybeLoaderError skip_id3(SeekableStream& stream);
    static MaybeLoaderError synchronize(SeekableStream& stream, size_t sample_index);
    static ErrorOr<MP3::Header, LoaderError> read_header(SeekableStream& stream, size_t sample_index);
    static ErrorOr<MP3::Header, LoaderError> synchronize_and_read_header(SeekableStream& stream, size_t sample_index);
    ErrorOr<MP3::Header, LoaderError> synchronize_and_read_header();
    MaybeLoaderError build_seek_table();
    ErrorOr<MP3::MP3Frame, LoaderError> read_next_frame();
    ErrorOr<MP3::MP3Frame, LoaderError> read_frame_data(MP3::Header const&);
    MaybeLoaderError read_side_information(MP3::MP3Frame&);
    ErrorOr<size_t, LoaderError> read_scale_factors(MP3::MP3Frame&, BigEndianInputBitStream& reservoir, size_t granule_index, size_t channel_index);
    MaybeLoaderError read_huffman_data(MP3::MP3Frame&, BigEndianInputBitStream& reservoir, size_t granule_index, size_t channel_index, size_t granule_bits_read);
    static AK::Array<float, 576> calculate_frame_exponents(MP3::MP3Frame const&, size_t granule_index, size_t channel_index);
    static void reorder_samples(MP3::Granule&, u32 sample_rate);
    static void reduce_alias(MP3::Granule&, size_t max_subband_index = 576);
    static void process_stereo(MP3::MP3Frame&, size_t granule_index);
    static void transform_samples_to_time(Array<float, 576> const& input, size_t input_offset, Array<float, 36>& output, MP3::BlockType block_type);
    static void synthesis(Array<float, 1024>& V, Array<float, 32>& samples, Array<float, 32>& result);
    static ReadonlySpan<MP3::Tables::ScaleFactorBand> get_scalefactor_bands(MP3::Granule const&, int samplerate);

    SeekTable m_seek_table;
    AK::Array<AK::Array<AK::Array<float, 18>, 32>, 2> m_last_values {};
    AK::Array<AK::Array<float, 1024>, 2> m_synthesis_buffer {};
    static DSP::MDCT<36> s_mdct_36;
    static DSP::MDCT<12> s_mdct_12;

    u32 m_sample_rate { 0 };
    u8 m_num_channels { 0 };
    PcmSampleFormat m_sample_format { PcmSampleFormat::Int16 };
    int m_total_samples { 0 };
    size_t m_loaded_samples { 0 };

    AllocatingMemoryStream m_bit_reservoir;
};

}
