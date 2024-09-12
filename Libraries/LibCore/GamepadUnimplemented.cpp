/*
 * Copyright (c) 2024, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Gamepad.h>

namespace Core {

ErrorOr<NonnullRefPtr<Gamepad>> Gamepad::create(StringView)
{
    return Error::from_errno(ENOTSUP);
}

}
