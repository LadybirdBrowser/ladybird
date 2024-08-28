/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Loader.h"
#include "QOATypes.h"
#include "SampleFormats.h"
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/Types.h>

namespace Audio {

// Decoder for the Quite Okay Audio (QOA) format.
// NOTE: The QOA format is not finalized yet and this decoder might not be fully spec-compliant as of 2023-02-02.
//
// https://github.com/phoboslab/qoa/blob/master/qoa.h
class QOALoaderPlugin : public LoaderPlugin {
public:
    explicit QOALoaderPlugin(NonnullOwnPtr<AK::SeekableStream> stream);
    virtual ~QOALoaderPlugin() override = default;

    static bool sniff(SeekableStream& stream);
    static ErrorOr<NonnullOwnPtr<LoaderPlugin>, LoaderError> create(NonnullOwnPtr<SeekableStream>);

    virtual ErrorOr<Vector<FixedArray<Sample>>, LoaderError> load_chunks(size_t samples_to_read_from_input) override;

    virtual MaybeLoaderError reset() override;
    virtual MaybeLoaderError seek(int sample_index) override;

    virtual int loaded_samples() override { return static_cast<int>(m_loaded_samples); }
    virtual int total_samples() override { return static_cast<int>(m_total_samples); }
    virtual u32 sample_rate() override { return m_sample_rate; }
    virtual u16 num_channels() override { return m_num_channels; }
    virtual ByteString format_name() override { return "Quite Okay Audio (.qoa)"; }
    virtual PcmSampleFormat pcm_format() override { return PcmSampleFormat::Int16; }

private:
    enum class IsFirstFrame : bool {
        Yes = true,
        No = false,
    };

    MaybeLoaderError initialize();
    MaybeLoaderError parse_header();

    MaybeLoaderError load_one_frame(Span<Sample>& target, IsFirstFrame is_first_frame = IsFirstFrame::No);
    // Updates predictor values in lms_state so the next slice can reuse the same state.
    MaybeLoaderError read_one_slice(QOA::LMSState& lms_state, Span<i16>& samples);
    static ALWAYS_INLINE QOA::UnpackedSlice unpack_slice(QOA::PackedSlice packed_slice);

    // QOA's division routine for scaling residuals before final quantization.
    static ALWAYS_INLINE i16 qoa_divide(i16 value, i16 scale_factor);

    // Because QOA has dynamic sample rate and channel count, we only use the sample rate and channel count from the first frame.
    u32 m_sample_rate { 0 };
    u8 m_num_channels { 0 };
    // If this is the case (the reference encoder even enforces it at the moment)
    bool m_has_uniform_channel_count { true };

    size_t m_loaded_samples { 0 };
    size_t m_total_samples { 0 };
};

}
