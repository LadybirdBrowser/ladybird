/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibCore/Gamepad.h>

namespace Core {

ErrorOr<Vector<String>> find_all_connected_gamepads();

}
