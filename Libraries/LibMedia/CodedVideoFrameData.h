/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>

namespace Media {

class CodedVideoFrameData {
public:
    explicit CodedVideoFrameData(Optional<AK::Duration> decode_timestamp = {})
        : m_decode_timestamp(decode_timestamp)
    {
    }

    Optional<AK::Duration> decode_timestamp() const { return m_decode_timestamp; }

private:
    Optional<AK::Duration> m_decode_timestamp;
};

}
