/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/MediaCapture/AudioOutputDevices.h>

namespace Media::Capture {

ErrorOr<Vector<AudioOutputDeviceInfo>> AudioOutputDevices::enumerate()
{
    Vector<AudioOutputDeviceInfo> result;
    return result;
}

}
