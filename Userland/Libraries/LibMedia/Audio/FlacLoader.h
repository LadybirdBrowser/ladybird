/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "FlacTypes.h"
#include "Loader.h"
#include <AK/BitStream.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/Types.h>

namespace Audio {

ALWAYS_INLINE u8 frame_channel_type_to_channel_count(FlacFrameChannelType channel_type);
// Sign-extend an arbitrary-size signed number to 64 bit signed
ALWAYS_INLINE i64 sign_extend(u32 n, u8 size);
// Decodes the sign representation method used in Rice coding.
// Numbers alternate between positive and negative: 0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, ...
ALWAYS_INLINE i32 rice_to_signed(u32 x);

// decoders
// read a UTF-8 encoded number, even if it is not a valid codepoint
ALWAYS_INLINE ErrorOr<u64> read_utf8_char(BigEndianInputBitStream& input);
// decode a single number encoded with exponential golomb encoding of the specified order
ALWAYS_INLINE ErrorOr<i32> decode_unsigned_exp_golomb(u8 order, BigEndianInputBitStream& bit_input);

// Loader for the Free Lossless Audio Codec (FLAC)
// This loader supports all audio features of FLAC, although audio from more than two channels is discarded.
// The loader currently supports the STREAMINFO, PADDING, and SEEKTABLE metadata blocks.
// See: https://xiph.org/flac/documentation_format_overview.html
//      https://xiph.org/flac/format.html (identical to IETF draft version 2)
//      https://datatracker.ietf.org/doc/html/draft-ietf-cellar-flac-02 (all section numbers refer to this specification)
//      https://datatracker.ietf.org/doc/html/draft-ietf-cellar-flac-03 (newer IETF draft that uses incompatible numberings and names)
class FlacLoaderPlugin : public LoaderPlugin {
public:
    explicit FlacLoaderPlugin(NonnullOwnPtr<SeekableStream> stream);
    virtual ~FlacLoaderPlugin() override = default;

    static bool sniff(SeekableStream& stream);
    static ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> create(NonnullOwnPtr<SeekableStream>);

    virtual ErrorOr<Vector<FixedArray<Sample>>, LoaderError> load_chunks(size_t samples_to_read_from_input) override;

    virtual MaybeLoaderError reset() override;
    virtual MaybeLoaderError seek(int sample_index) override;

    virtual int loaded_samples() override { return static_cast<int>(m_loaded_samples); }
    virtual int total_samples() override { return static_cast<int>(m_total_samples); }
    virtual u32 sample_rate() override { return m_sample_rate; }
    virtual u16 num_channels() override { return m_num_channels; }
    virtual ByteString format_name() override { return "FLAC (.flac)"; }
    virtual PcmSampleFormat pcm_format() override { return m_sample_format; }

    bool is_fixed_blocksize_stream() const { return m_min_block_size == m_max_block_size; }
    bool sample_count_unknown() const { return m_total_samples == 0; }

private:
    MaybeLoaderError initialize();
    MaybeLoaderError parse_header();
    // Either returns the metadata block or sets error message.
    // Additionally, increments m_data_start_location past the read meta block.
    ErrorOr<FlacRawMetadataBlock, LoaderError> next_meta_block(BigEndianInputBitStream& bit_input);
    // Fetches and returns the next FLAC frame.
    LoaderSamples next_frame();
    // Helper of next_frame that fetches a sub frame's header
    ErrorOr<FlacSubframeHeader, LoaderError> next_subframe_header(BigEndianInputBitStream& bit_input, u8 channel_index);
    // Helper of next_frame that decompresses a subframe
    ErrorOr<void, LoaderError> parse_subframe(Vector<i64>& samples, FlacSubframeHeader& subframe_header, BigEndianInputBitStream& bit_input);
    // Subframe-internal data decoders (heavy lifting)
    ErrorOr<Vector<i64>, LoaderError> decode_fixed_lpc(FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input);
    ErrorOr<Vector<i64>, LoaderError> decode_verbatim(FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input);
    ErrorOr<void, LoaderError> decode_custom_lpc(Vector<i64>& decoded, FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input);
    MaybeLoaderError decode_residual(Vector<i64>& decoded, FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input);
    // decode a single rice partition that has its own rice parameter
    ALWAYS_INLINE ErrorOr<Vector<i64>, LoaderError> decode_rice_partition(u8 partition_type, u32 partitions, u32 partition_index, FlacSubframeHeader& subframe, BigEndianInputBitStream& bit_input);
    MaybeLoaderError load_seektable(FlacRawMetadataBlock&);
    // Note that failing to read a Vorbis comment block is not treated as an error of the FLAC loader, since metadata is optional.
    void load_vorbis_comment(FlacRawMetadataBlock&);
    MaybeLoaderError load_picture(FlacRawMetadataBlock&);

    // Converters for special coding used in frame headers
    ALWAYS_INLINE ErrorOr<u32, LoaderError> convert_sample_count_code(u8 sample_count_code);
    ALWAYS_INLINE ErrorOr<u32, LoaderError> convert_sample_rate_code(u8 sample_rate_code);
    ALWAYS_INLINE ErrorOr<u8, LoaderError> convert_bit_depth_code(u8 bit_depth_code);

    bool should_insert_seekpoint_at(u64 sample_index) const;

    // Data obtained directly from the FLAC metadata: many values have specific bit counts
    u32 m_sample_rate { 0 };    // 20 bit
    u8 m_num_channels { 0 };    // 3 bit
    u8 m_bits_per_sample { 0 }; // 5 bits for the integer bit depth
    // Externally visible format; the smallest integer format that's larger than the precise bit depth.
    PcmSampleFormat m_sample_format;
    // Blocks are units of decoded audio data
    u16 m_min_block_size { 0 };
    u16 m_max_block_size { 0 };
    // Frames are units of encoded audio data, both of these are 24-bit
    u32 m_min_frame_size { 0 }; // 24 bit
    u32 m_max_frame_size { 0 }; // 24 bit
    u64 m_total_samples { 0 };  // 36 bit
    u8 m_md5_checksum[128 / 8]; // 128 bit (!)
    size_t m_loaded_samples { 0 };

    // keep track of the start of the data in the FLAC stream to seek back more easily
    u64 m_data_start_location { 0 };
    Optional<FlacFrameHeader> m_current_frame;
    u64 m_current_sample_or_frame { 0 };
    SeekTable m_seektable;

    // Keep around a few temporary buffers whose allocated space can be reused.
    // This is an empirical optimization since allocations and deallocations take a lot of time in the decoder.
    mutable Vector<Vector<i64>, 2> m_subframe_buffers;
};

}
