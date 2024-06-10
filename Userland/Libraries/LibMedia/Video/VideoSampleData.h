/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Time.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Sample.h>

namespace Media::Video {

class VideoSampleData {
public:
    VideoSampleData(CodingIndependentCodePoints container_cicp)
        : m_container_cicp(container_cicp)
    {
    }

    CodingIndependentCodePoints container_cicp() const { return m_container_cicp; }

private:
    CodingIndependentCodePoints m_container_cicp;
};

}
