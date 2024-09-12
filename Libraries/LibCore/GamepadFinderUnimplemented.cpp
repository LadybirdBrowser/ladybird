/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/GamepadFinder.h>

namespace Core {

ErrorOr<Vector<String>> find_all_connected_gamepads()
{
    return Error::from_errno(ENOTSUP);
}

}
