/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <AudioServer/AudioInputDeviceInfo.h>

namespace AudioServer {

class AudioInputDeviceManager {
public:
    static Vector<AudioInputDeviceInfo> enumerate_devices();
};

}
