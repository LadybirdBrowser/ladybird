/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/Audio/TimeStretcher.h>

namespace Audio {

class WSOLATimeStretcher final : public TimeStretcher {
public:
    static ErrorOr<NonnullOwnPtr<TimeStretcher>> create(SampleSpecification);
    virtual ~WSOLATimeStretcher() override;

    virtual i64 preroll_frame_count() const override;
    virtual void flush(AK::Duration media_start_timestamp, i64 output_start_frame_index) override;
    virtual void set_rate(float) override;
    virtual void push_block(Media::AudioBlock const&) override;
    virtual Media::DecoderErrorOr<Media::AudioBlock> retrieve_block() override;
    virtual void signal_end_of_stream() override;

private:
    struct Impl;

    explicit WSOLATimeStretcher(NonnullOwnPtr<Impl>);
    NonnullOwnPtr<Impl> m_impl;
};

}
