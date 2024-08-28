/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Types.h>
#include <AK/Vector.h>

namespace Audio {

// Small helper to resample from one playback rate to another
// This isn't really "smart", in that we just insert (or drop) samples.
// Should do better...
template<typename SampleType>
class ResampleHelper {
public:
    ResampleHelper(u32 source, u32 target)
        : m_source(source)
        , m_target(target)
    {
        VERIFY(source > 0);
        VERIFY(target > 0);
    }

    // To be used as follows:
    // while the resampler doesn't need a new sample, read_sample(current) and store the resulting samples.
    // as long as the resampler needs a new sample, process_sample(current)

    // Stores a new sample
    void process_sample(SampleType sample_l, SampleType sample_r)
    {
        m_last_sample_l = sample_l;
        m_last_sample_r = sample_r;
        m_current_ratio += m_target;
    }

    // Assigns the given sample to its correct value and returns false if there is a new sample required
    bool read_sample(SampleType& next_l, SampleType& next_r)
    {
        if (m_current_ratio >= m_source) {
            m_current_ratio -= m_source;
            next_l = m_last_sample_l;
            next_r = m_last_sample_r;
            return true;
        }

        return false;
    }

    template<ArrayLike<SampleType> Samples>
    ErrorOr<Vector<SampleType>> try_resample(Samples&& to_resample)
    {
        Vector<SampleType> resampled;
        TRY(try_resample_into_end(resampled, forward<Samples>(to_resample)));
        return resampled;
    }

    template<ArrayLike<SampleType> Samples, size_t vector_inline_capacity = 0>
    ErrorOr<void> try_resample_into_end(Vector<SampleType, vector_inline_capacity>& destination, Samples&& to_resample)
    {
        float ratio = (m_source > m_target) ? static_cast<float>(m_source) / m_target : static_cast<float>(m_target) / m_source;
        TRY(destination.try_ensure_capacity(destination.size() + to_resample.size() * ratio));
        for (auto sample : to_resample) {
            process_sample(sample, sample);

            while (read_sample(sample, sample))
                destination.unchecked_append(sample);
        }
        return {};
    }

    template<ArrayLike<SampleType> Samples>
    Vector<SampleType> resample(Samples&& to_resample)
    {
        return MUST(try_resample(forward<Samples>(to_resample)));
    }

    void reset()
    {
        m_current_ratio = 0;
        m_last_sample_l = {};
        m_last_sample_r = {};
    }

    u32 source() const { return m_source; }
    u32 target() const { return m_target; }

private:
    u32 const m_source;
    u32 const m_target;
    u32 m_current_ratio { 0 };
    SampleType m_last_sample_l {};
    SampleType m_last_sample_r {};
};

}
