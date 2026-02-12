/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/MediaCapture/AudioInputDevices.h>

namespace Media::Capture {

ErrorOr<Vector<AudioInputDeviceInfo>> AudioInputDevices::enumerate()
{
    Vector<AudioInputDeviceInfo> result;
    return result;
}

}
