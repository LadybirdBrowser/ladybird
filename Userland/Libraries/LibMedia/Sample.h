/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Time.h>

namespace Media {

class Sample {
public:
    virtual ~Sample() = default;

    virtual bool is_video_sample() const { return false; }
};

}
