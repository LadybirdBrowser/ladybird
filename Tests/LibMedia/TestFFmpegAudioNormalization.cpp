/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/NumericLimits.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibTest/TestCase.h>

template<typename Integer>
requires(IsIntegral<Integer>)
static void append_little_endian(ByteBuffer& buffer, Integer value)
{
    using Unsigned = MakeUnsigned<Integer>;
    Unsigned unsigned_value = static_cast<Unsigned>(value);
    auto bytes = buffer.must_get_bytes_for_writing(sizeof(Integer));
    for (size_t byte_index = 0; byte_index < sizeof(Integer); ++byte_index)
        bytes[byte_index] = static_cast<u8>(unsigned_value >> (byte_index * 8));
}

template<typename Sample>
requires(IsSigned<Sample>)
static ByteBuffer make_square_wav(size_t sample_count)
{
    constexpr u16 channel_count = 1;
    constexpr u32 sample_rate = 8'000;
    constexpr u16 bits_per_sample = sizeof(Sample) * 8;
    constexpr u16 block_align = channel_count * sizeof(Sample);
    constexpr u32 byte_rate = sample_rate * block_align;

    u32 data_size = static_cast<u32>(sample_count * sizeof(Sample));
    u32 riff_chunk_size = 36 + data_size;

    ByteBuffer buffer;
    buffer.ensure_capacity(44 + data_size);

    buffer.append("RIFF"sv.bytes());
    append_little_endian(buffer, riff_chunk_size);
    buffer.append("WAVE"sv.bytes());

    buffer.append("fmt "sv.bytes());
    append_little_endian(buffer, 16u);
    append_little_endian(buffer, static_cast<u16>(1));
    append_little_endian(buffer, channel_count);
    append_little_endian(buffer, sample_rate);
    append_little_endian(buffer, byte_rate);
    append_little_endian(buffer, block_align);
    append_little_endian(buffer, bits_per_sample);

    buffer.append("data"sv.bytes());
    append_little_endian(buffer, data_size);

    for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        Sample sample = (sample_index % 2 == 0) ? NumericLimits<Sample>::min() : NumericLimits<Sample>::max();
        append_little_endian(buffer, sample);
    }

    return buffer;
}

template<typename Sample>
requires(IsSigned<Sample>)
static void decode_and_expect()
{
    Core::EventLoop loop;

    constexpr size_t sample_count = 2048;
    auto wav_data = make_square_wav<Sample>(sample_count);
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(wav_data);
    auto demuxer = MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));

    auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    auto provider = TRY_OR_FAIL(Media::AudioDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track.release_value()));

    bool reached_end_of_stream = false;
    provider->set_error_handler([&](Media::DecoderError&& error) {
        if (error.category() == Media::DecoderErrorCategory::EndOfStream) {
            reached_end_of_stream = true;
            return;
        }
        FAIL("An error occurred while decoding generated WAV data.");
    });
    provider->start();

    bool saw_negative_full_scale_sample = false;
    bool saw_positive_peak_sample = false;
    size_t decoded_sample_count = 0;

    MonotonicTime deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(1);
    while (MonotonicTime::now_coarse() < deadline) {
        auto block = provider->retrieve_block();
        if (block.is_empty()) {
            if (reached_end_of_stream)
                break;
        } else {
            for (float sample : block.data()) {
                EXPECT(sample >= -1.0f);
                if constexpr (sizeof(Sample) >= sizeof(i32))
                    EXPECT(sample <= 1.0f);
                else
                    EXPECT(sample < 1.0f);
                if (sample == -1.0f)
                    saw_negative_full_scale_sample = true;
                if (sample > 0.0f)
                    saw_positive_peak_sample = true;
            }
            decoded_sample_count += block.sample_count();
        }

        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    EXPECT(reached_end_of_stream);
    EXPECT_EQ(decoded_sample_count, sample_count);
    EXPECT(saw_negative_full_scale_sample);
    EXPECT(saw_positive_peak_sample);
}

TEST_CASE(signed_16bit_pcm_to_float)
{
    decode_and_expect<i16>();
}

TEST_CASE(signed_32bit_pcm_to_float)
{
    decode_and_expect<i32>();
}

TEST_CASE(signed_64bit_pcm_to_float)
{
    decode_and_expect<i64>();
}
