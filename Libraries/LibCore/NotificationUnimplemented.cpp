/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <LibCore/Notification.h>

namespace Core {

ErrorOr<NonnullOwnPtr<PlatformNotification>> PlatformNotification::create()
{
    return Error::from_errno(ENOTSUP);
}

}
