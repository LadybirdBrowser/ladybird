/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <AudioServer/AudioOutputDeviceInfo.h>

namespace AudioServer {

class AudioOutputDeviceManager {
public:
    static Vector<AudioOutputDeviceInfo> enumerate_devices();
};

}
