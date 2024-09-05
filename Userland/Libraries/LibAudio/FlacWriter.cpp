/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FlacWriter.h"
#include <AK/BitStream.h>
#include <AK/DisjointChunks.h>
#include <AK/Endian.h>
#include <AK/IntegralMath.h>
#include <AK/MemoryStream.h>
#include <AK/Statistics.h>
#include <LibAudio/Metadata.h>
#include <LibAudio/VorbisComment.h>
#include <LibCrypto/Checksum/ChecksummingStream.h>

namespace Audio {

ErrorOr<NonnullOwnPtr<FlacWriter>> FlacWriter::create(NonnullOwnPtr<SeekableStream> stream, u32 sample_rate, u8 num_channels, u16 bits_per_sample)
{
    auto writer = TRY(AK::adopt_nonnull_own_or_enomem(new (nothrow) FlacWriter(move(stream))));
    TRY(writer->set_bits_per_sample(bits_per_sample));
    TRY(writer->set_sample_rate(sample_rate));
    TRY(writer->set_num_channels(num_channels));
    return writer;
}

FlacWriter::FlacWriter(NonnullOwnPtr<SeekableStream> stream)
    : m_stream(move(stream))
{
}

FlacWriter::~FlacWriter()
{
    if (m_state != WriteState::FullyFinalized)
        (void)finalize();
}

ErrorOr<void> FlacWriter::finalize()
{
    if (m_state == WriteState::FullyFinalized)
        return Error::from_string_literal("File is already finalized");

    if (m_state == WriteState::HeaderUnwritten)
        TRY(finalize_header_format());

    if (!m_sample_buffer.is_empty())
        TRY(write_frame());

    {
        // 1 byte metadata block header + 3 bytes size + 2*2 bytes min/max block size
        TRY(m_stream->seek(m_streaminfo_start_index + 8, AK::SeekMode::SetPosition));
        BigEndianOutputBitStream bit_stream { MaybeOwned<Stream> { *m_stream } };
        TRY(bit_stream.write_bits(m_min_frame_size, 24));
        TRY(bit_stream.write_bits(m_max_frame_size, 24));
        TRY(bit_stream.write_bits(m_sample_rate, 20));
        TRY(bit_stream.write_bits(m_num_channels - 1u, 3));
        TRY(bit_stream.write_bits(m_bits_per_sample - 1u, 5));
        TRY(bit_stream.write_bits(m_sample_count, 36));
        TRY(bit_stream.align_to_byte_boundary());
    }

    TRY(flush_seektable());

    // TODO: Write the audio data MD5 to the header.

    m_stream->close();

    m_state = WriteState::FullyFinalized;
    return {};
}

ErrorOr<void> FlacWriter::finalize_header_format()
{
    if (m_state != WriteState::HeaderUnwritten)
        return Error::from_string_literal("Header format is already finalized");
    TRY(write_header());
    m_state = WriteState::FormatFinalized;
    return {};
}

ErrorOr<void> FlacWriter::set_num_channels(u8 num_channels)
{
    if (m_state != WriteState::HeaderUnwritten)
        return Error::from_string_literal("Header format is already finalized");
    if (num_channels > 8)
        return Error::from_string_literal("FLAC doesn't support more than 8 channels");

    m_num_channels = num_channels;
    return {};
}

ErrorOr<void> FlacWriter::set_sample_rate(u32 sample_rate)
{
    if (m_state != WriteState::HeaderUnwritten)
        return Error::from_string_literal("Header format is already finalized");

    m_sample_rate = sample_rate;
    return {};
}

ErrorOr<void> FlacWriter::set_bits_per_sample(u16 bits_per_sample)
{
    if (m_state != WriteState::HeaderUnwritten)
        return Error::from_string_literal("Header format is already finalized");
    if (bits_per_sample < 8 || bits_per_sample > 32)
        return Error::from_string_literal("FLAC only supports bits per sample between 8 and 32");

    m_bits_per_sample = bits_per_sample;
    return {};
}

ErrorOr<void> FlacWriter::set_metadata(Metadata const& metadata)
{
    AllocatingMemoryStream vorbis_stream;
    TRY(write_vorbis_comment(metadata, vorbis_stream));

    auto vorbis_data = TRY(vorbis_stream.read_until_eof());
    FlacRawMetadataBlock vorbis_block {
        .is_last_block = false,
        .type = FlacMetadataBlockType::VORBIS_COMMENT,
        .length = static_cast<u32>(vorbis_data.size()),
        .data = move(vorbis_data),
    };
    return add_metadata_block(move(vorbis_block), 0);
}

size_t FlacWriter::max_number_of_seekpoints() const
{
    if (m_last_padding.has_value())
        return m_last_padding->size / flac_seekpoint_size;

    if (!m_cached_metadata_blocks.is_empty() && m_cached_metadata_blocks.last().type == FlacMetadataBlockType::PADDING)
        return m_cached_metadata_blocks.last().length / flac_seekpoint_size;

    return 0;
}

void FlacWriter::sample_count_hint(size_t sample_count)
{
    constexpr StringView oom_warning = "FLAC Warning: Couldn't use sample hint to reserve {} bytes padding; ignoring hint."sv;

    auto const samples_per_seekpoint = m_sample_rate * seekpoint_period_seconds;
    auto seekpoint_count = round_to<size_t>(static_cast<double>(sample_count) / samples_per_seekpoint);
    // Round seekpoint count down to an even number, so that the seektable byte size is divisible by 4.
    // One seekpoint is 18 bytes, which isn't divisible by 4.
    seekpoint_count &= ~1;
    auto const seektable_size = seekpoint_count * flac_seekpoint_size;

    // Only modify the trailing padding block; other padding blocks are intentionally untouched.
    if (!m_cached_metadata_blocks.is_empty() && m_cached_metadata_blocks.last().type == FlacMetadataBlockType::PADDING) {
        auto padding_block = m_cached_metadata_blocks.last();
        auto result = padding_block.data.try_resize(seektable_size);
        padding_block.length = padding_block.data.size();
        // Fuzzers and inputs with wrong large sample counts often hit this.
        if (result.is_error())
            dbgln(oom_warning, seektable_size);
    } else {
        auto empty_buffer = ByteBuffer::create_zeroed(seektable_size);
        if (empty_buffer.is_error()) {
            dbgln(oom_warning, seektable_size);
            return;
        }
        FlacRawMetadataBlock padding {
            .is_last_block = true,
            .type = FlacMetadataBlockType::PADDING,
            .length = static_cast<u32>(empty_buffer.value().size()),
            .data = empty_buffer.release_value(),
        };
        // If we can't add padding, we're out of luck.
        (void)add_metadata_block(move(padding));
    }
}

ErrorOr<void> FlacWriter::write_header()
{
    ByteBuffer data;
    // STREAMINFO is always exactly 34 bytes long.
    TRY(data.try_resize(34));
    BigEndianOutputBitStream header_stream { TRY(try_make<FixedMemoryStream>(data.bytes())) };

    // Duplication on purpose:
    // Minimum frame size.
    TRY(header_stream.write_bits(block_size, 16));
    // Maximum frame size.
    TRY(header_stream.write_bits(block_size, 16));
    // Leave the frame sizes as unknown for now.
    TRY(header_stream.write_bits(0u, 24));
    TRY(header_stream.write_bits(0u, 24));

    TRY(header_stream.write_bits(m_sample_rate, 20));
    TRY(header_stream.write_bits(m_num_channels - 1u, 3));
    TRY(header_stream.write_bits(m_bits_per_sample - 1u, 5));
    // Leave the sample count as unknown for now.
    TRY(header_stream.write_bits(0u, 36));

    // TODO: Calculate the MD5 signature of all of the audio data.
    auto md5 = TRY(ByteBuffer::create_zeroed(128u / 8u));
    TRY(header_stream.write_until_depleted(md5));

    FlacRawMetadataBlock streaminfo_block = {
        .is_last_block = true,
        .type = FlacMetadataBlockType::STREAMINFO,
        .length = static_cast<u32>(data.size()),
        .data = move(data),
    };
    TRY(add_metadata_block(move(streaminfo_block), 0));

    // Add default padding if necessary.
    if (m_cached_metadata_blocks.last().type != FlacMetadataBlockType::PADDING) {
        auto padding_data = ByteBuffer::create_zeroed(default_padding);
        if (!padding_data.is_error()) {
            TRY(add_metadata_block({
                .is_last_block = true,
                .type = FlacMetadataBlockType::PADDING,
                .length = default_padding,
                .data = padding_data.release_value(),
            }));
        }
    }

    TRY(m_stream->write_until_depleted(flac_magic.bytes()));
    m_streaminfo_start_index = TRY(m_stream->tell());

    for (size_t i = 0; i < m_cached_metadata_blocks.size(); ++i) {
        auto& block = m_cached_metadata_blocks[i];
        // Correct is_last_block flag here to avoid index shenanigans in add_metadata_block.
        auto const is_last_block = i == m_cached_metadata_blocks.size() - 1;
        block.is_last_block = is_last_block;
        if (is_last_block) {
            m_last_padding = LastPadding {
                .start = TRY(m_stream->tell()),
                .size = block.length,
            };
        }

        TRY(write_metadata_block(block));
    }

    m_cached_metadata_blocks.clear();
    m_frames_start_index = TRY(m_stream->tell());
    return {};
}

ErrorOr<void> FlacWriter::add_metadata_block(FlacRawMetadataBlock block, Optional<size_t> insertion_index)
{
    if (m_state != WriteState::HeaderUnwritten)
        return Error::from_string_literal("Metadata blocks can only be added before the header is finalized");

    if (insertion_index.has_value())
        TRY(m_cached_metadata_blocks.try_insert(insertion_index.value(), move(block)));
    else
        TRY(m_cached_metadata_blocks.try_append(move(block)));

    return {};
}

ErrorOr<void> FlacWriter::write_metadata_block(FlacRawMetadataBlock& block)
{
    if (m_state == WriteState::FormatFinalized) {
        if (!m_last_padding.has_value())
            return Error::from_string_literal("No (more) padding available to write block into");

        auto const last_padding = m_last_padding.release_value();
        if (block.length > last_padding.size)
            return Error::from_string_literal("Late metadata block doesn't fit in available padding");

        auto const current_position = TRY(m_stream->tell());
        ScopeGuard guard = [&] { (void)m_stream->seek(current_position, SeekMode::SetPosition); };
        TRY(m_stream->seek(last_padding.start, SeekMode::SetPosition));

        // No more padding after this: the new block is the last.
        auto new_size = last_padding.size - block.length;
        if (new_size == 0)
            block.is_last_block = true;

        TRY(m_stream->write_value(block));

        // If the size is zero, we don't need to write a new padding block.
        // If the size is between 1 and 3, we have empty space that cannot be marked with an empty padding block, so we must abort.
        // Other code should make sure that this never happens; e.g. our seektable only has sizes divisible by 4 anyways.
        // If the size is 4, we have no padding, but the padding block header can be written without any subsequent payload.
        if (new_size >= 4) {
            FlacRawMetadataBlock new_padding_block {
                .is_last_block = true,
                .type = FlacMetadataBlockType::PADDING,
                .length = static_cast<u32>(new_size),
                .data = TRY(ByteBuffer::create_zeroed(new_size)),
            };
            m_last_padding = LastPadding {
                .start = TRY(m_stream->tell()),
                .size = new_size,
            };
            TRY(m_stream->write_value(new_padding_block));
        } else if (new_size != 0) {
            return Error::from_string_literal("Remaining padding is not divisible by 4, there will be some stray zero bytes!");
        }

        return {};
    }

    return m_stream->write_value(block);
}

ErrorOr<void> FlacRawMetadataBlock::write_to_stream(Stream& stream) const
{
    BigEndianOutputBitStream bit_stream { MaybeOwned<Stream> { stream } };
    TRY(bit_stream.write_bits(static_cast<u8>(is_last_block), 1));
    TRY(bit_stream.write_bits(to_underlying(type), 7));
    TRY(bit_stream.write_bits(length, 24));

    VERIFY(data.size() == length);
    TRY(bit_stream.write_until_depleted(data));
    return {};
}

ErrorOr<void> FlacWriter::flush_seektable()
{
    if (m_cached_seektable.size() == 0)
        return {};

    auto max_seekpoints = max_number_of_seekpoints();
    if (max_seekpoints < m_cached_seektable.size()) {
        dbgln("FLAC Warning: There are {} seekpoints, but we only have space for {}. Some seekpoints will be dropped.", m_cached_seektable.size(), max_seekpoints);
        // Drop seekpoints in regular intervals to space out the loss of seek precision.
        auto const points_to_drop = m_cached_seektable.size() - max_seekpoints;
        auto const drop_interval = static_cast<double>(m_cached_seektable.size()) / static_cast<double>(points_to_drop);
        double ratio = 0.;
        for (size_t i = 0; i < m_cached_seektable.size(); ++i) {
            // Avoid dropping the first seekpoint.
            if (ratio > drop_interval) {
                m_cached_seektable.seek_points().remove(i);
                --i;
                ratio -= drop_interval;
            }
            ++ratio;
        }
        // Account for integer division imprecisions.
        if (max_seekpoints < m_cached_seektable.size())
            m_cached_seektable.seek_points().shrink(max_seekpoints);
    }

    auto seektable_data = TRY(ByteBuffer::create_zeroed(m_cached_seektable.size() * flac_seekpoint_size));
    FixedMemoryStream seektable_stream { seektable_data.bytes() };

    for (auto const& seekpoint : m_cached_seektable.seek_points()) {
        // https://www.ietf.org/archive/id/draft-ietf-cellar-flac-08.html#name-seekpoint
        TRY(seektable_stream.write_value<BigEndian<u64>>(seekpoint.sample_index));
        TRY(seektable_stream.write_value<BigEndian<u64>>(seekpoint.byte_offset));
        // This is probably wrong for the last frame, but it doesn't seem to matter.
        TRY(seektable_stream.write_value<BigEndian<u16>>(block_size));
    }

    FlacRawMetadataBlock seektable {
        .is_last_block = false,
        .type = FlacMetadataBlockType::SEEKTABLE,
        .length = static_cast<u32>(seektable_data.size()),
        .data = move(seektable_data),
    };
    return write_metadata_block(seektable);
}

// If the given sample count is uncommon, this function will return one of the uncommon marker block sizes.
// The caller has to handle and add these later manually.
static BlockSizeCategory to_common_block_size(u16 sample_count)
{
    switch (sample_count) {
    case 192:
        return BlockSizeCategory::S192;
    case 576:
        return BlockSizeCategory::S576;
    case 1152:
        return BlockSizeCategory::S1152;
    case 2304:
        return BlockSizeCategory::S2304;
    case 4608:
        return BlockSizeCategory::S4608;
    case 256:
        return BlockSizeCategory::S256;
    case 512:
        return BlockSizeCategory::S512;
    case 1024:
        return BlockSizeCategory::S1024;
    case 2048:
        return BlockSizeCategory::S2048;
    case 4096:
        return BlockSizeCategory::S4096;
    case 8192:
        return BlockSizeCategory::S8192;
    case 16384:
        return BlockSizeCategory::S16384;
    case 32768:
        return BlockSizeCategory::S32768;
    }
    if (sample_count - 1 <= 0xff)
        return BlockSizeCategory::Uncommon8Bits;
    // Data type guarantees that 16-bit storage is possible.
    return BlockSizeCategory::Uncommon16Bits;
}

static ByteBuffer to_utf8(u64 value)
{
    ByteBuffer buffer;
    if (value < 0x7f) {
        buffer.append(static_cast<u8>(value));
    } else if (value < 0x7ff) {
        buffer.append(static_cast<u8>(0b110'00000 | (value >> 6)));
        buffer.append(static_cast<u8>(0b10'000000 | (value & 0b111111)));
    } else if (value < 0xffff) {
        buffer.append(static_cast<u8>(0b1110'0000 | (value >> 12)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 6) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 0) & 0b111111)));
    } else if (value < 0x1f'ffff) {
        buffer.append(static_cast<u8>(0b11110'000 | (value >> 18)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 12) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 6) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 0) & 0b111111)));
    } else if (value < 0x3ff'ffff) {
        buffer.append(static_cast<u8>(0b111110'00 | (value >> 24)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 18) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 12) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 6) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 0) & 0b111111)));
    } else if (value < 0x7fff'ffff) {
        buffer.append(static_cast<u8>(0b1111110'0 | (value >> 30)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 24) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 18) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 12) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 6) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 0) & 0b111111)));
    } else if (value < 0xf'ffff'ffff) {
        buffer.append(static_cast<u8>(0b11111110));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 30) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 24) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 18) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 12) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 6) & 0b111111)));
        buffer.append(static_cast<u8>(0b10'000000 | ((value >> 0) & 0b111111)));
    } else {
        // Anything larger is illegal even in expanded UTF-8, but FLAC only passes 32-bit values anyways.
        VERIFY_NOT_REACHED();
    }
    return buffer;
}

ErrorOr<void> FlacFrameHeader::write_to_stream(Stream& stream) const
{
    Crypto::Checksum::ChecksummingStream<FlacFrameHeaderCRC> checksumming_stream { MaybeOwned<Stream> { stream } };
    BigEndianOutputBitStream bit_stream { MaybeOwned<Stream> { checksumming_stream } };
    TRY(bit_stream.write_bits(0b11111111111110u, 14));
    TRY(bit_stream.write_bits(0u, 1));
    TRY(bit_stream.write_bits(to_underlying(blocking_strategy), 1));

    auto common_block_size = to_common_block_size(sample_count);
    TRY(bit_stream.write_bits(to_underlying(common_block_size), 4));

    // We always store sample rate in the file header.
    TRY(bit_stream.write_bits(0u, 4));
    TRY(bit_stream.write_bits(to_underlying(channels), 4));
    // We always store bit depth in the file header.
    TRY(bit_stream.write_bits(0u, 3));
    // Reserved zero bit.
    TRY(bit_stream.write_bits(0u, 1));

    auto coded_number = to_utf8(sample_or_frame_index);
    TRY(bit_stream.write_until_depleted(coded_number));

    if (common_block_size == BlockSizeCategory::Uncommon8Bits)
        TRY(bit_stream.write_value(static_cast<u8>(sample_count - 1)));
    if (common_block_size == BlockSizeCategory::Uncommon16Bits)
        TRY(bit_stream.write_value(BigEndian<u16>(static_cast<u16>(sample_count - 1))));

    // Ensure that the checksum is calculated correctly.
    TRY(bit_stream.align_to_byte_boundary());
    auto checksum = checksumming_stream.digest();
    TRY(bit_stream.write_value(checksum));

    return {};
}

ErrorOr<void> FlacWriter::write_samples(ReadonlySpan<Sample> samples)
{
    if (m_state == WriteState::FullyFinalized)
        return Error::from_string_literal("File is already finalized");

    auto remaining_samples = samples;
    while (remaining_samples.size() > 0) {
        if (m_sample_buffer.size() == block_size) {
            TRY(write_frame());
            m_sample_buffer.clear();
        }
        auto amount_to_copy = min(remaining_samples.size(), m_sample_buffer.capacity() - m_sample_buffer.size());
        auto current_buffer_size = m_sample_buffer.size();
        TRY(m_sample_buffer.try_resize_and_keep_capacity(current_buffer_size + amount_to_copy));
        remaining_samples.copy_trimmed_to(m_sample_buffer.span().slice(current_buffer_size));
        remaining_samples = remaining_samples.slice(amount_to_copy);
    }

    // Ensure that the buffer is flushed if possible.
    if (m_sample_buffer.size() == block_size) {
        TRY(write_frame());
        m_sample_buffer.clear();
    }

    return {};
}

ErrorOr<void> FlacWriter::write_frame()
{
    auto frame_samples = move(m_sample_buffer);
    // De-interleave and integer-quantize subframes.
    float sample_rescale = static_cast<float>(1 << (m_bits_per_sample - 1));
    auto subframe_samples = Vector<Vector<i64, block_size>>();
    TRY(subframe_samples.try_resize_and_keep_capacity(m_num_channels));
    for (auto const& sample : frame_samples) {
        TRY(subframe_samples[0].try_append(static_cast<i64>(sample.left * sample_rescale)));
        // FIXME: We don't have proper data for any channels past 2.
        for (auto i = 1; i < m_num_channels; ++i)
            TRY(subframe_samples[i].try_append(static_cast<i64>(sample.right * sample_rescale)));
    }

    auto channel_type = static_cast<FlacFrameChannelType>(m_num_channels - 1);

    if (channel_type == FlacFrameChannelType::Stereo) {
        auto const& left_channel = subframe_samples[0];
        auto const& right_channel = subframe_samples[1];
        Vector<i64, block_size> mid_channel;
        Vector<i64, block_size> side_channel;
        TRY(mid_channel.try_ensure_capacity(left_channel.size()));
        TRY(side_channel.try_ensure_capacity(left_channel.size()));
        for (auto i = 0u; i < left_channel.size(); ++i) {
            auto mid = (left_channel[i] + right_channel[i]) / 2;
            auto side = left_channel[i] - right_channel[i];
            mid_channel.unchecked_append(mid);
            side_channel.unchecked_append(side);
        }

        AK::Statistics<i64, AK::DisjointSpans<i64>> normal_costs {
            AK::DisjointSpans<i64> { { subframe_samples[0], subframe_samples[1] } }
        };
        AK::Statistics<i64, AK::DisjointSpans<i64>> correlated_costs {
            AK::DisjointSpans<i64> { { mid_channel, side_channel } }
        };

        if (correlated_costs.standard_deviation() < normal_costs.standard_deviation()) {
            dbgln_if(FLAC_ENCODER_DEBUG, "Using channel coupling since sample stddev {} is better than {}", correlated_costs.standard_deviation(), normal_costs.standard_deviation());
            channel_type = FlacFrameChannelType::MidSideStereo;
            subframe_samples[0] = move(mid_channel);
            subframe_samples[1] = move(side_channel);
        }
    }

    auto const sample_index = m_sample_count;
    auto const frame_start_byte = TRY(write_frame_for(subframe_samples, channel_type));

    // Insert a seekpoint if necessary.
    auto const seekpoint_period_samples = m_sample_rate * seekpoint_period_seconds;
    auto const last_seekpoint = m_cached_seektable.seek_point_before(sample_index);
    if (!last_seekpoint.has_value() || static_cast<double>(sample_index - last_seekpoint->sample_index) >= seekpoint_period_samples) {
        dbgln_if(FLAC_ENCODER_DEBUG, "Inserting seekpoint at sample index {} frame start {}", sample_index, frame_start_byte);
        TRY(m_cached_seektable.insert_seek_point({
            .sample_index = sample_index,
            .byte_offset = frame_start_byte - m_frames_start_index,
        }));
    }

    return {};
}

ErrorOr<size_t> FlacWriter::write_frame_for(ReadonlySpan<Vector<i64, block_size>> subblock, FlacFrameChannelType channel_type)
{
    auto sample_count = subblock.first().size();

    FlacFrameHeader header {
        .sample_rate = m_sample_rate,
        .sample_count = static_cast<u16>(sample_count),
        .sample_or_frame_index = static_cast<u32>(m_current_frame),
        .blocking_strategy = BlockingStrategy::Fixed,
        .channels = channel_type,
        .bit_depth = static_cast<u8>(m_bits_per_sample),
        // Calculated for us during header write.
        .checksum = 0,
    };

    auto frame_stream = Crypto::Checksum::ChecksummingStream<IBMCRC> { MaybeOwned<Stream> { *m_stream } };

    auto frame_start_offset = TRY(m_stream->tell());
    TRY(frame_stream.write_value(header));

    BigEndianOutputBitStream bit_stream { MaybeOwned<Stream> { frame_stream } };
    for (auto i = 0u; i < subblock.size(); ++i) {
        auto const& subframe = subblock[i];
        auto bits_per_sample = m_bits_per_sample;
        // Side channels need an extra bit per sample.
        if ((i == 1 && (channel_type == FlacFrameChannelType::LeftSideStereo || channel_type == FlacFrameChannelType::MidSideStereo))
            || (i == 0 && channel_type == FlacFrameChannelType::RightSideStereo)) {
            bits_per_sample++;
        }

        TRY(write_subframe(subframe.span(), bit_stream, bits_per_sample));
    }

    TRY(bit_stream.align_to_byte_boundary());
    auto frame_crc = frame_stream.digest();
    dbgln_if(FLAC_ENCODER_DEBUG, "Frame {:4} CRC: {:04x}", m_current_frame, frame_crc);
    TRY(frame_stream.write_value<AK::BigEndian<u16>>(frame_crc));

    auto frame_end_offset = TRY(m_stream->tell());
    auto frame_size = frame_end_offset - frame_start_offset;
    m_max_frame_size = max(m_max_frame_size, frame_size);
    m_min_frame_size = min(m_min_frame_size, frame_size);

    m_current_frame++;
    m_sample_count += sample_count;

    return frame_start_offset;
}

ErrorOr<void> FlacWriter::write_subframe(ReadonlySpan<i64> subframe, BigEndianOutputBitStream& bit_stream, u8 bits_per_sample)
{
    // The current subframe encoding strategy is as follows:
    // - Check if the subframe is constant; use constant encoding in this case.
    // - Try all fixed predictors and record the resulting residuals.
    // - Estimate their encoding cost by taking the sum of all absolute logarithmic residuals,
    //   which is an accurate estimate of the final encoded size of the residuals.
    // - Accurately estimate the encoding cost of a verbatim subframe.
    // - Select the encoding strategy with the lowest cost out of this selection.

    auto constant_value = subframe[0];
    auto is_constant = true;
    for (auto const sample : subframe) {
        if (sample != constant_value) {
            is_constant = false;
            break;
        }
    }

    if (is_constant) {
        dbgln_if(FLAC_ENCODER_DEBUG, "Encoding constant frame with value {}", constant_value);
        TRY(bit_stream.write_bits(1u, 0));
        TRY(bit_stream.write_bits(to_underlying(FlacSubframeType::Constant), 6));
        TRY(bit_stream.write_bits(1u, 0));
        TRY(bit_stream.write_bits(bit_cast<u64>(constant_value), bits_per_sample));
        return {};
    }

    auto verbatim_cost_bits = subframe.size() * bits_per_sample;

    Optional<FlacLPCEncodedSubframe> best_lpc_subframe;
    auto current_min_cost = verbatim_cost_bits;
    for (auto order : { FlacFixedLPC::Zero, FlacFixedLPC::One, FlacFixedLPC::Two, FlacFixedLPC::Three, FlacFixedLPC::Four }) {
        // Too many warm-up samples would be required; the lower-level encoding procedures assume that this was checked.
        if (to_underlying(order) > subframe.size())
            continue;

        auto encode_result = TRY(encode_fixed_lpc(order, subframe, current_min_cost, bits_per_sample));
        if (encode_result.has_value() && encode_result.value().residual_cost_bits < current_min_cost) {
            current_min_cost = encode_result.value().residual_cost_bits;
            best_lpc_subframe = encode_result.release_value();
        }
    }

    // No LPC encoding was better than verbatim.
    if (!best_lpc_subframe.has_value()) {
        dbgln_if(FLAC_ENCODER_DEBUG, "Best subframe type was Verbatim; encoding {} samples at {} bps = {} bits", subframe.size(), m_bits_per_sample, verbatim_cost_bits);
        TRY(write_verbatim_subframe(subframe, bit_stream, bits_per_sample));
    } else {
        dbgln_if(FLAC_ENCODER_DEBUG, "Best subframe type was Fixed LPC order {} (estimated cost {} bits); encoding {} samples", to_underlying(best_lpc_subframe->coefficients.get<FlacFixedLPC>()), best_lpc_subframe->residual_cost_bits, subframe.size());
        TRY(write_lpc_subframe(best_lpc_subframe.release_value(), bit_stream, bits_per_sample));
    }

    return {};
}

ErrorOr<Optional<FlacLPCEncodedSubframe>> FlacWriter::encode_fixed_lpc(FlacFixedLPC order, ReadonlySpan<i64> subframe, size_t current_min_cost, u8 bits_per_sample)
{
    FlacLPCEncodedSubframe lpc {
        .warm_up_samples = Vector<i64> { subframe.trim(to_underlying(order)) },
        .coefficients = order,
        .residuals {},
        // Warm-up sample cost.
        .residual_cost_bits = to_underlying(order) * bits_per_sample,
        .single_partition_optimal_order {},
    };
    TRY(lpc.residuals.try_ensure_capacity(subframe.size() - to_underlying(order)));

    Vector<i64> predicted;
    TRY(predicted.try_resize_and_keep_capacity(subframe.size()));
    lpc.warm_up_samples.span().copy_trimmed_to(predicted);

    // NOTE: Although we can't interrupt the prediction if the corresponding residuals would become too bad,
    //       we don't need to branch on the order in every loop during prediction, meaning this shouldn't cost us much.
    predict_fixed_lpc(order, subframe, predicted);

    // There isn’t really a way of computing an LPC’s cost without performing most of the calculations, including a Rice parameter search.
    // This is nevertheless optimized in multiple ways, so that we always bail out once we are sure no improvements can be made.
    auto extra_residual_cost = NumericLimits<size_t>::max();
    // Keep track of when we want to estimate costs again. We don't do this for every new residual since it's an expensive procedure.
    // The likelihood for misprediction is pretty high for large orders; start with a later index for them.
    auto next_cost_estimation_index = min(subframe.size() - 1, first_residual_estimation * (to_underlying(order) + 1));
    for (auto i = to_underlying(order); i < subframe.size(); ++i) {
        auto residual = subframe[i] - predicted[i];
        if (!AK::is_within_range<i32>(residual)) {
            dbgln_if(FLAC_ENCODER_DEBUG, "Bailing from Fixed LPC order {} due to residual overflow ({} is outside the 32-bit range)", to_underlying(order), residual);
            return Optional<FlacLPCEncodedSubframe> {};
        }
        lpc.residuals.append(residual);

        if (i >= next_cost_estimation_index) {
            // Find best exponential Golomb order.
            // Storing this in the LPC data allows us to automatically reuse the computation during LPC encoding.
            // FIXME: Use more than one partition to improve compression.
            // FIXME: Investigate whether this can be estimated “good enough” to improve performance at the cost of compression strength.
            // Especially at larger sample counts, it is unlikely that we will find a different optimal order.
            // Therefore, use a zig-zag search around the previous optimal order.
            extra_residual_cost = NumericLimits<size_t>::max();
            auto start_order = lpc.single_partition_optimal_order;
            size_t useless_parameters = 0;
            size_t steps = 0;
            constexpr auto max_rice_parameter = AK::exp2(4) - 1;
            for (auto offset = 0; start_order + offset < max_rice_parameter || start_order - offset >= 0; ++offset) {
                for (auto factor : { -1, 1 }) {
                    auto k = start_order + factor * offset;
                    if (k >= max_rice_parameter || k < 0)
                        continue;

                    auto order_cost = count_exp_golomb_bits_in(k, lpc.residuals);
                    if (order_cost < extra_residual_cost) {
                        extra_residual_cost = order_cost;
                        lpc.single_partition_optimal_order = k;
                    } else {
                        useless_parameters++;
                    }
                    steps++;
                    // Don’t do 0 twice.
                    if (offset == 0)
                        break;
                }
                // If we found enough useless parameters, we probably won't find useful ones anymore.
                // The only exception is the first ever parameter search, where we search everything.
                if (useless_parameters >= useless_parameter_threshold && start_order != 0)
                    break;
            }

            // Min cost exceeded; bail out.
            if (lpc.residual_cost_bits + extra_residual_cost > current_min_cost) {
                dbgln_if(FLAC_ENCODER_DEBUG, "  Bailing from Fixed LPC order {} at sample index {} and cost {} (best {})", to_underlying(order), i, lpc.residual_cost_bits + extra_residual_cost, current_min_cost);
                return Optional<FlacLPCEncodedSubframe> {};
            }

            // Figure out when to next estimate costs.
            auto estimated_bits_per_residual = static_cast<double>(extra_residual_cost) / static_cast<double>(i);
            auto estimated_residuals_for_min_cost = static_cast<double>(current_min_cost) / estimated_bits_per_residual;
            auto unchecked_next_cost_estimation_index = AK::round_to<size_t>(estimated_residuals_for_min_cost * (1 - residual_cost_margin));
            // Check either at the estimated residual, or the next residual if that is in the past, or the last residual.
            next_cost_estimation_index = min(subframe.size() - 1, max(unchecked_next_cost_estimation_index, i + min_residual_estimation_step));
            dbgln_if(FLAC_ENCODER_DEBUG, "    {} {:4} Estimate cost/residual {:.1f} (param {:2} after {:2} steps), will hit at {:6.1f}, jumping to {:4} (sanitized to {:4})", to_underlying(order), i, estimated_bits_per_residual, lpc.single_partition_optimal_order, steps, estimated_residuals_for_min_cost, unchecked_next_cost_estimation_index, next_cost_estimation_index);
        }
    }

    lpc.residual_cost_bits += extra_residual_cost;
    return lpc;
}

void predict_fixed_lpc(FlacFixedLPC order, ReadonlySpan<i64> samples, Span<i64> predicted_output)
{
    switch (order) {
    case FlacFixedLPC::Zero:
        // s_0(t) = 0
        for (auto i = to_underlying(order); i < predicted_output.size(); ++i)
            predicted_output[i] += 0;
        break;
    case FlacFixedLPC::One:
        // s_1(t) = s(t-1)
        for (auto i = to_underlying(order); i < predicted_output.size(); ++i)
            predicted_output[i] += samples[i - 1];
        break;
    case FlacFixedLPC::Two:
        // s_2(t) = 2s(t-1) - s(t-2)
        for (auto i = to_underlying(order); i < predicted_output.size(); ++i)
            predicted_output[i] += 2 * samples[i - 1] - samples[i - 2];
        break;
    case FlacFixedLPC::Three:
        // s_3(t) = 3s(t-1) - 3s(t-2) + s(t-3)
        for (auto i = to_underlying(order); i < predicted_output.size(); ++i)
            predicted_output[i] += 3 * samples[i - 1] - 3 * samples[i - 2] + samples[i - 3];
        break;
    case FlacFixedLPC::Four:
        // s_4(t) = 4s(t-1) - 6s(t-2) + 4s(t-3) - s(t-4)
        for (auto i = to_underlying(order); i < predicted_output.size(); ++i)
            predicted_output[i] += 4 * samples[i - 1] - 6 * samples[i - 2] + 4 * samples[i - 3] - samples[i - 4];
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://www.ietf.org/archive/id/draft-ietf-cellar-flac-08.html#name-verbatim-subframe
ErrorOr<void> FlacWriter::write_verbatim_subframe(ReadonlySpan<i64> subframe, BigEndianOutputBitStream& bit_stream, u8 bits_per_sample)
{
    TRY(bit_stream.write_bits(0u, 1));
    TRY(bit_stream.write_bits(to_underlying(FlacSubframeType::Verbatim), 6));
    TRY(bit_stream.write_bits(0u, 1));
    for (auto const& sample : subframe)
        TRY(bit_stream.write_bits(bit_cast<u64>(sample), bits_per_sample));

    return {};
}

// https://www.ietf.org/archive/id/draft-ietf-cellar-flac-08.html#name-fixed-predictor-subframe
ErrorOr<void> FlacWriter::write_lpc_subframe(FlacLPCEncodedSubframe lpc_subframe, BigEndianOutputBitStream& bit_stream, u8 bits_per_sample)
{
    // Reserved.
    TRY(bit_stream.write_bits(0u, 1));
    // 9.2.1 Subframe header (https://www.ietf.org/archive/id/draft-ietf-cellar-flac-08.html#name-subframe-header)
    u8 encoded_type;
    if (lpc_subframe.coefficients.has<FlacFixedLPC>())
        encoded_type = to_underlying(lpc_subframe.coefficients.get<FlacFixedLPC>()) + to_underlying(FlacSubframeType::Fixed);
    else
        encoded_type = lpc_subframe.coefficients.get<Vector<i64>>().size() - 1 + to_underlying(FlacSubframeType::LPC);

    TRY(bit_stream.write_bits(encoded_type, 6));
    // No wasted bits per sample (unnecessary for the vast majority of data).
    TRY(bit_stream.write_bits(0u, 1));

    for (auto const& warm_up_sample : lpc_subframe.warm_up_samples)
        TRY(bit_stream.write_bits(bit_cast<u64>(warm_up_sample), bits_per_sample));

    // 4-bit Rice parameters.
    TRY(bit_stream.write_bits(0b00u, 2));
    // Only one partition (2^0 = 1).
    TRY(bit_stream.write_bits(0b0000u, 4));
    TRY(write_rice_partition(lpc_subframe.single_partition_optimal_order, lpc_subframe.residuals, bit_stream));

    return {};
}

ErrorOr<void> FlacWriter::write_rice_partition(u8 k, ReadonlySpan<i64> residuals, BigEndianOutputBitStream& bit_stream)
{
    TRY(bit_stream.write_bits(k, 4));

    for (auto const& residual : residuals)
        TRY(encode_unsigned_exp_golomb(k, static_cast<i32>(residual), bit_stream));

    return {};
}

u32 signed_to_rice(i32 x)
{
    // Implements (x < 0 ? -1 : 0) + 2 * abs(x) in about half as many instructions.
    // The reference encoder’s implementation is known to be the fastest on -O2/3 clang and gcc:
    // x << 1 = multiply by 2.
    // For negative numbers, x >> 31 will create an all-ones XOR mask, meaning that the number will be inverted.
    // In two's complement this is -value - 1, exactly what we need.
    // For positive numbers, x >> 31 == 0.
    return static_cast<u32>((x << 1) ^ (x >> 31));
}

// Adopted from https://github.com/xiph/flac/blob/28e4f0528c76b296c561e922ba67d43751990599/src/libFLAC/bitwriter.c#L727
ErrorOr<void> encode_unsigned_exp_golomb(u8 k, i32 value, BigEndianOutputBitStream& bit_stream)
{
    auto zigzag_encoded = signed_to_rice(value);
    auto msbs = zigzag_encoded >> k;
    auto pattern = 1u << k;
    pattern |= zigzag_encoded & ((1 << k) - 1);

    TRY(bit_stream.write_bits(0u, msbs));
    TRY(bit_stream.write_bits(pattern, k + 1));

    return {};
}

// Adopted from count_rice_bits_in_partition():
// https://github.com/xiph/flac/blob/28e4f0528c76b296c561e922ba67d43751990599/src/libFLAC/stream_encoder.c#L4299
size_t count_exp_golomb_bits_in(u8 k, ReadonlySpan<i64> residuals)
{
    // Exponential Golomb order size (4).
    // One unary stop bit and the entire exponential Golomb parameter for every residual.
    size_t partition_bits = 4 + (1 + k) * residuals.size();

    // Bit magic to compute the amount of leading unary bits.
    for (auto const& residual : residuals)
        partition_bits += (static_cast<u32>((residual << 1) ^ (residual >> 31)) >> k);

    return partition_bits;
}

}
