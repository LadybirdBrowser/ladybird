/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/TimeZoneWatcher.h>

namespace Core {

ErrorOr<NonnullOwnPtr<TimeZoneWatcher>> TimeZoneWatcher::create()
{
    return Error::from_errno(ENOTSUP);
}

}
