/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Time.h>

#include "VideoSampleData.h"

namespace Media {

class Sample final {
public:
    using AuxiliaryData = Variant<VideoSampleData>;

    Sample(Duration timestamp, ReadonlyBytes data, AuxiliaryData auxiliary_data)
        : m_timestamp(timestamp)
        , m_data(data)
        , m_auxiliary_data(auxiliary_data)
    {
    }

    Duration timestamp() const { return m_timestamp; }
    ReadonlyBytes const& data() const { return m_data; }
    AuxiliaryData const& auxiliary_data() const { return m_auxiliary_data; }

private:
    Duration m_timestamp;
    ReadonlyBytes m_data;
    AuxiliaryData m_auxiliary_data;
};

}
