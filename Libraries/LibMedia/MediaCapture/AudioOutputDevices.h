/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaCapture/AudioOutputDeviceInfo.h>

namespace Media::Capture {

class MEDIA_API AudioOutputDevices {
public:
    static ErrorOr<Vector<AudioOutputDeviceInfo>> enumerate();
};

}
