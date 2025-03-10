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

    Sample(AK::Duration timestamp, ByteBuffer data, AuxiliaryData auxiliary_data)
        : m_timestamp(timestamp)
        , m_data(data)
        , m_auxiliary_data(auxiliary_data)
    {
    }

    AK::Duration timestamp() const { return m_timestamp; }
    ByteBuffer const& data() const { return m_data; }
    AuxiliaryData const& auxiliary_data() const { return m_auxiliary_data; }

private:
    AK::Duration m_timestamp;
    ByteBuffer m_data;
    AuxiliaryData m_auxiliary_data;
};

}
