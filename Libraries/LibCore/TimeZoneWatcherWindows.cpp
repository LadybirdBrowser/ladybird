/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibCore/TimeZoneWatcher.h>

#if !defined(AK_OS_WINDOWS)
static_assert(false, "This file must only be used for Windows");
#endif

namespace Core {

class TimeZoneWatcherImpl final : public TimeZoneWatcher {
public:
    static ErrorOr<NonnullOwnPtr<TimeZoneWatcherImpl>> create()
    {
        return adopt_own(*new TimeZoneWatcherImpl());
    }

private:
    TimeZoneWatcherImpl() = default;
};

ErrorOr<NonnullOwnPtr<TimeZoneWatcher>> TimeZoneWatcher::create()
{
    return TimeZoneWatcherImpl::create();
}

}
