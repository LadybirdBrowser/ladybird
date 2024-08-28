/*
 * Copyright (c) 2020, William McPherson <willmcpherson2@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Encoder.h"
#include "Sample.h"
#include "SampleFormats.h"
#include <AK/ByteString.h>
#include <AK/Noncopyable.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <LibCore/File.h>
#include <LibCore/Forward.h>

namespace Audio {

class WavWriter : public Encoder {
    AK_MAKE_NONCOPYABLE(WavWriter);
    AK_MAKE_NONMOVABLE(WavWriter);

public:
    static ErrorOr<NonnullOwnPtr<WavWriter>> create_from_file(StringView path, u32 sample_rate = 44100, u16 num_channels = 2, PcmSampleFormat sample_format = PcmSampleFormat::Int16);
    WavWriter(u32 sample_rate = 44100, u16 num_channels = 2, PcmSampleFormat sample_format = PcmSampleFormat::Int16);
    ~WavWriter();

    virtual ErrorOr<void> write_samples(ReadonlySpan<Sample> samples) override;
    virtual ErrorOr<void> finalize() override;

    ErrorOr<void> set_file(StringView path);

private:
    ErrorOr<void> write_header();
    OwnPtr<Core::OutputBufferedFile> m_file;
    bool m_finalized { false };

    u32 m_sample_rate;
    u16 m_num_channels;
    PcmSampleFormat m_sample_format;
    u32 m_data_sz { 0 };
};

}
