/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Encoder.h"
#include "FlacTypes.h"
#include "Forward.h"
#include "GenericTypes.h"
#include "Sample.h"
#include "SampleFormats.h"
#include <AK/MaybeOwned.h>
#include <AK/Noncopyable.h>
#include <AK/RefPtr.h>
#include <AK/Stream.h>
#include <AK/StringView.h>
#include <LibCore/Forward.h>

namespace Audio {

// Encodes the sign representation method used in Rice coding.
// Numbers alternate between positive and negative: 0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, ...
ALWAYS_INLINE u32 signed_to_rice(i32 x);

// Encode a single number encoded with exponential golomb encoding of the specified order (k).
ALWAYS_INLINE ErrorOr<void> encode_unsigned_exp_golomb(u8 k, i32 value, BigEndianOutputBitStream& bit_stream);

size_t count_exp_golomb_bits_in(u8 k, ReadonlySpan<i64> residuals);

void predict_fixed_lpc(FlacFixedLPC order, ReadonlySpan<i64> samples, Span<i64> predicted_output);

// A simple FLAC encoder that writes FLAC files compatible with the streamable subset.
// The encoder currently has the following simple output properties:
// FIXME: All frames have a fixed sample size, see below.
// FIXME: All frames are encoded with the best fixed LPC predictor.
// FIXME: All residuals are encoded in one Rice partition.
class FlacWriter : public Encoder {
    AK_MAKE_NONCOPYABLE(FlacWriter);
    AK_MAKE_NONMOVABLE(FlacWriter);

    /// Tunable static parameters. Please try to improve these; only some have already been well-tuned!

    // Constant block size.
    static constexpr size_t block_size = 1024;
    // Used as a percentage to check residual costs before the estimated "necessary" estimation point.
    // We usually over-estimate residual costs, so this prevents us from overshooting the actual bail point.
    static constexpr double residual_cost_margin = 0.07;
    // At what sample index to first estimate residuals, so that the residual parameter can "stabilize" through more encoded values.
    static constexpr size_t first_residual_estimation = 16;
    // How many samples to advance at minimum before estimating residuals again.
    static constexpr size_t min_residual_estimation_step = 20;
    // After how many useless (i.e. worse than current optimal) Rice parameters to abort parameter search.
    // Note that due to the zig-zag search, we start with searching the parameters that are most likely to be good.
    static constexpr size_t useless_parameter_threshold = 2;
    // How often a seek point is inserted.
    static constexpr double seekpoint_period_seconds = 2.0;
    // Default padding reserved for seek points; enough for almost 4 minutes of audio.
    static constexpr size_t default_padding = 2048;

    enum class WriteState {
        // Header has not been written at all, audio data cannot be written.
        HeaderUnwritten,
        // Header was written, i.e. sample format is finalized,
        // but audio data has not been finalized and therefore some header information is still missing.
        FormatFinalized,
        // File is fully finalized, no more sample data can be written.
        FullyFinalized,
    };

public:
    static ErrorOr<NonnullOwnPtr<FlacWriter>> create(NonnullOwnPtr<SeekableStream> stream, u32 sample_rate = 44100, u8 num_channels = 2, u16 bits_per_sample = 16);
    virtual ~FlacWriter();

    virtual ErrorOr<void> write_samples(ReadonlySpan<Sample> samples) override;

    virtual ErrorOr<void> finalize() override;

    u32 sample_rate() const { return m_sample_rate; }
    u8 num_channels() const { return m_num_channels; }
    PcmSampleFormat sample_format() const { return integer_sample_format_for(m_bits_per_sample).value(); }
    Stream const& output_stream() const { return *m_stream; }

    ErrorOr<void> set_num_channels(u8 num_channels);
    ErrorOr<void> set_sample_rate(u32 sample_rate);
    ErrorOr<void> set_bits_per_sample(u16 bits_per_sample);

    // The FLAC encoder by default tries to reserve some space for seek points,
    // but that may not be enough if more than approximately four minutes of audio are stored.
    // The sample count hint can be used to instruct the FLAC encoder on how much space to reserve for seek points,
    // which will both reduce the padding for small files and allow the FLAC encoder to write seek points at the end of large files.
    virtual void sample_count_hint(size_t sample_count) override;

    virtual ErrorOr<void> set_metadata(Metadata const& metadata) override;

    ErrorOr<void> finalize_header_format();

private:
    FlacWriter(NonnullOwnPtr<SeekableStream>);
    ErrorOr<void> write_header();

    ErrorOr<void> write_frame();
    // Returns the frame start byte offset, to be used for creating a seektable.
    ErrorOr<size_t> write_frame_for(ReadonlySpan<Vector<i64, block_size>> subblock, FlacFrameChannelType channel_type);
    ErrorOr<void> write_subframe(ReadonlySpan<i64> subframe, BigEndianOutputBitStream& bit_stream, u8 bits_per_sample);
    ErrorOr<void> write_lpc_subframe(FlacLPCEncodedSubframe lpc_subframe, BigEndianOutputBitStream& bit_stream, u8 bits_per_sample);
    ErrorOr<void> write_verbatim_subframe(ReadonlySpan<i64> subframe, BigEndianOutputBitStream& bit_stream, u8 bits_per_sample);
    // Assumes 4-bit k for now.
    ErrorOr<void> write_rice_partition(u8 k, ReadonlySpan<i64> residuals, BigEndianOutputBitStream& bit_stream);

    // Aborts encoding once the costs exceed the previous minimum, thereby speeding up the encoder's parameter search.
    // In this case, an empty Optional is returned.
    ErrorOr<Optional<FlacLPCEncodedSubframe>> encode_fixed_lpc(FlacFixedLPC order, ReadonlySpan<i64> subframe, size_t current_min_cost, u8 bits_per_sample);

    ErrorOr<void> add_metadata_block(FlacRawMetadataBlock block, Optional<size_t> insertion_index = {});
    // Depending on whether the header is finished or not, we either write to the current position for an unfinished header,
    // or we write to the start of the last padding and adjust that padding block.
    ErrorOr<void> write_metadata_block(FlacRawMetadataBlock& block);
    // Determine how many seekpoints we can write depending on the size of our final padding.
    size_t max_number_of_seekpoints() const;
    ErrorOr<void> flush_seektable();

    NonnullOwnPtr<SeekableStream> m_stream;
    WriteState m_state { WriteState::HeaderUnwritten };

    Vector<Sample, block_size> m_sample_buffer {};
    size_t m_current_frame { 0 };

    u32 m_sample_rate;
    u8 m_num_channels;
    u16 m_bits_per_sample;

    // Data updated during encoding; written to the header at the end.
    u32 m_max_frame_size { 0 };
    u32 m_min_frame_size { NumericLimits<u32>::max() };
    size_t m_sample_count { 0 };
    // Remember where the STREAMINFO block was written in the stream.
    size_t m_streaminfo_start_index;
    // Start of the first frame, used for calculating seektable byte offsets.
    size_t m_frames_start_index;

    struct LastPadding {
        size_t start;
        size_t size;
    };
    // Remember last PADDING block data, since we overwrite part of it with "late" metadata blocks.
    Optional<LastPadding> m_last_padding;

    // Raw metadata blocks that will be written out before header finalization.
    Vector<FlacRawMetadataBlock> m_cached_metadata_blocks;

    // The full seektable, may be fully or partially written.
    SeekTable m_cached_seektable {};
};

}
