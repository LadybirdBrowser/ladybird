/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibCore/Export.h>

namespace Core {

class CORE_API PlatformNotification {
    AK_MAKE_NONCOPYABLE(PlatformNotification);

public:
    static ErrorOr<NonnullOwnPtr<PlatformNotification>> create();

    virtual ~PlatformNotification() = default;

    virtual ErrorOr<void> show_notification(String const& title) = 0;

protected:
    PlatformNotification() = default;
};

}
