/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "QOALoader.h"
#include "Loader.h"
#include "LoaderError.h"
#include "QOATypes.h"
#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Endian.h>
#include <AK/FixedArray.h>
#include <AK/MemoryStream.h>
#include <AK/Stream.h>
#include <AK/Types.h>
#include <LibCore/File.h>

namespace Audio {

QOALoaderPlugin::QOALoaderPlugin(NonnullOwnPtr<AK::SeekableStream> stream)
    : LoaderPlugin(move(stream))
{
}

bool QOALoaderPlugin::sniff(SeekableStream& stream)
{
    auto maybe_qoa = stream.read_value<BigEndian<u32>>();
    return !maybe_qoa.is_error() && maybe_qoa.value() == QOA::magic;
}

ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> QOALoaderPlugin::create(NonnullOwnPtr<SeekableStream> stream)
{
    auto loader = make<QOALoaderPlugin>(move(stream));
    TRY(loader->initialize());
    return loader;
}

MaybeLoaderError QOALoaderPlugin::initialize()
{
    TRY(parse_header());
    TRY(reset());
    return {};
}

MaybeLoaderError QOALoaderPlugin::parse_header()
{
    u32 header_magic = TRY(m_stream->read_value<BigEndian<u32>>());
    if (header_magic != QOA::magic)
        return LoaderError { LoaderError::Category::Format, 0, "QOA header: Magic number must be 'qoaf'" };

    m_total_samples = TRY(m_stream->read_value<BigEndian<u32>>());

    return {};
}

MaybeLoaderError QOALoaderPlugin::load_one_frame(Span<Sample>& target, IsFirstFrame is_first_frame)
{
    QOA::FrameHeader header = TRY(m_stream->read_value<QOA::FrameHeader>());

    if (header.num_channels > 8)
        dbgln("QOALoader: Warning: QOA frame at {} has more than 8 channels ({}), this is not supported by the reference implementation.", TRY(m_stream->tell()) - sizeof(QOA::FrameHeader), header.num_channels);
    if (header.num_channels == 0)
        return LoaderError { LoaderError::Category::Format, TRY(m_stream->tell()), "QOA frame: Number of channels must be greater than 0" };
    if (header.sample_count > QOA::max_frame_samples)
        return LoaderError { LoaderError::Category::Format, TRY(m_stream->tell()), "QOA frame: Too many samples in frame" };

    // We weren't given a large enough buffer; signal that we didn't write anything and return.
    if (header.sample_count > target.size()) {
        target = target.trim(0);
        TRY(m_stream->seek(-sizeof(QOA::frame_header_size), AK::SeekMode::FromCurrentPosition));
        return {};
    }

    target = target.trim(header.sample_count);

    auto lms_states = TRY(FixedArray<QOA::LMSState>::create(header.num_channels));
    for (size_t channel = 0; channel < header.num_channels; ++channel) {
        auto history_packed = TRY(m_stream->read_value<BigEndian<u64>>());
        auto weights_packed = TRY(m_stream->read_value<BigEndian<u64>>());
        lms_states[channel] = { history_packed, weights_packed };
    }

    // We pre-allocate very large arrays here, but that's the last allocation of the QOA loader!
    // Everything else is just shuffling data around.
    // (We will also be using all of the arrays in every frame but the last one.)
    auto channels = TRY((FixedArray<Array<i16, QOA::max_frame_samples>>::create(header.num_channels)));

    // There's usually (and at maximum) 256 slices per channel, but less at the very end.
    // If the final slice would be partial, we still need to decode it; integer division would tell us that this final slice doesn't exist.
    auto const slice_count = static_cast<size_t>(ceil(static_cast<double>(header.sample_count) / static_cast<double>(QOA::slice_samples)));
    VERIFY(slice_count <= QOA::max_slices_per_frame);

    // Observe the loop nesting: Slices are channel-interleaved.
    for (size_t slice = 0; slice < slice_count; ++slice) {
        for (size_t channel = 0; channel < header.num_channels; ++channel) {
            auto slice_samples = channels[channel].span().slice(slice * QOA::slice_samples, QOA::slice_samples);
            TRY(read_one_slice(lms_states[channel], slice_samples));
        }
    }

    if (is_first_frame == IsFirstFrame::Yes) {
        m_num_channels = header.num_channels;
        m_sample_rate = header.sample_rate;
    } else {
        if (m_sample_rate != header.sample_rate)
            return LoaderError { LoaderError::Category::Unimplemented, TRY(m_stream->tell()), "QOA: Differing sample rate in non-initial frame" };
        if (m_num_channels != header.num_channels)
            m_has_uniform_channel_count = false;
    }

    switch (header.num_channels) {
    case 1:
        for (size_t sample = 0; sample < header.sample_count; ++sample)
            target[sample] = Sample { static_cast<float>(channels[0][sample]) / static_cast<float>(NumericLimits<i16>::max()) };
        break;
    // FIXME: Combine surround channels sensibly, FlacLoader has the same simplification at the moment.
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    default:
        for (size_t sample = 0; sample < header.sample_count; ++sample) {
            target[sample] = {
                static_cast<float>(channels[0][sample]) / static_cast<float>(NumericLimits<i16>::max()),
                static_cast<float>(channels[1][sample]) / static_cast<float>(NumericLimits<i16>::max()),
            };
        }
        break;
    }

    return {};
}

ErrorOr<Vector<FixedArray<Sample>>, LoaderError> QOALoaderPlugin::load_chunks(size_t samples_to_read_from_input)
{
    ssize_t const remaining_samples = static_cast<ssize_t>(m_total_samples - m_loaded_samples);
    if (remaining_samples <= 0)
        return Vector<FixedArray<Sample>> {};
    size_t const samples_to_read = min(samples_to_read_from_input, remaining_samples);
    auto is_first_frame = m_loaded_samples == 0 ? IsFirstFrame::Yes : IsFirstFrame::No;

    Vector<FixedArray<Sample>> frames;
    size_t current_loaded_samples = 0;

    while (current_loaded_samples < samples_to_read) {
        auto samples = TRY(FixedArray<Sample>::create(QOA::max_frame_samples));
        auto slice_to_load_into = samples.span();
        TRY(this->load_one_frame(slice_to_load_into, is_first_frame));
        is_first_frame = IsFirstFrame::No;
        VERIFY(slice_to_load_into.size() <= QOA::max_frame_samples);
        current_loaded_samples += slice_to_load_into.size();
        if (slice_to_load_into.size() != samples.size()) {
            auto smaller_samples = TRY(FixedArray<Sample>::create(slice_to_load_into));
            samples.swap(smaller_samples);
        }
        TRY(frames.try_append(move(samples)));

        if (slice_to_load_into.size() != samples.size())
            break;
    }
    m_loaded_samples += current_loaded_samples;

    return frames;
}

MaybeLoaderError QOALoaderPlugin::reset()
{
    TRY(m_stream->seek(QOA::header_size, AK::SeekMode::SetPosition));
    m_loaded_samples = 0;
    // Read the first frame, then seek back to the beginning. This is necessary since the first frame contains the sample rate and channel count.
    auto frame_samples = TRY(FixedArray<Sample>::create(QOA::max_frame_samples));
    auto span = frame_samples.span();
    TRY(load_one_frame(span, IsFirstFrame::Yes));

    TRY(m_stream->seek(QOA::header_size, AK::SeekMode::SetPosition));
    m_loaded_samples = 0;
    return {};
}

MaybeLoaderError QOALoaderPlugin::seek(int sample_index)
{
    if (sample_index == 0 && m_loaded_samples == 0)
        return {};
    // A QOA file consists of 8 bytes header followed by a number of usually fixed-size frames.
    // This fixed bitrate allows us to seek in constant time.
    if (!m_has_uniform_channel_count)
        return LoaderError { LoaderError::Category::Unimplemented, TRY(m_stream->tell()), "QOA with non-uniform channel count is currently not seekable"sv };
    /// FIXME: Change the Loader API to use size_t.
    VERIFY(sample_index >= 0);
    // We seek to the frame "before"; i.e. the frame that contains that sample.
    auto const frame_of_sample = static_cast<size_t>(AK::floor<double>(static_cast<double>(sample_index) / static_cast<double>(QOA::max_frame_samples)));
    auto const frame_size = QOA::frame_header_size + m_num_channels * (QOA::lms_state_size + sizeof(QOA::PackedSlice) * QOA::max_slices_per_frame);
    auto const byte_index = QOA::header_size + frame_of_sample * frame_size;
    TRY(m_stream->seek(byte_index, AK::SeekMode::SetPosition));
    m_loaded_samples = frame_of_sample * QOA::max_frame_samples;
    return {};
}

MaybeLoaderError QOALoaderPlugin::read_one_slice(QOA::LMSState& lms_state, Span<i16>& samples)
{
    VERIFY(samples.size() == QOA::slice_samples);

    auto packed_slice = TRY(m_stream->read_value<BigEndian<u64>>());
    auto unpacked_slice = unpack_slice(packed_slice);

    for (size_t i = 0; i < QOA::slice_samples; ++i) {
        auto const residual = unpacked_slice.residuals[i];
        auto const predicted = lms_state.predict();
        auto const dequantized = QOA::dequantization_table[unpacked_slice.scale_factor_index][residual];
        auto const reconstructed = clamp(predicted + dequantized, QOA::sample_minimum, QOA::sample_maximum);
        samples[i] = static_cast<i16>(reconstructed);
        lms_state.update(reconstructed, dequantized);
    }

    return {};
}

QOA::UnpackedSlice QOALoaderPlugin::unpack_slice(QOA::PackedSlice packed_slice)
{
    size_t const scale_factor_index = (packed_slice >> 60) & 0b1111;
    Array<u8, 20> residuals = {};
    auto shifted_slice = packed_slice << 4;

    for (size_t i = 0; i < QOA::slice_samples; ++i) {
        residuals[i] = static_cast<u8>((shifted_slice >> 61) & 0b111);
        shifted_slice <<= 3;
    }

    return {
        .scale_factor_index = scale_factor_index,
        .residuals = residuals,
    };
}

i16 QOALoaderPlugin::qoa_divide(i16 value, i16 scale_factor)
{
    auto const reciprocal = QOA::reciprocal_table[scale_factor];
    auto const n = (value * reciprocal + (1 << 15)) >> 16;
    // Rounding away from zero gives better quantization for small values.
    auto const n_rounded = n + (static_cast<int>(value > 0) - static_cast<int>(value < 0)) - (static_cast<int>(n > 0) - static_cast<int>(n < 0));
    return static_cast<i16>(n_rounded);
}

}
