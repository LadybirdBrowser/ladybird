/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibCore/TimeZoneWatcher.h>

#if !defined(AK_OS_MACOS)
static_assert(false, "This file must only be used for macOS");
#endif

#include <CoreFoundation/CoreFoundation.h>

namespace Core {

class TimeZoneWatcherImpl final : public TimeZoneWatcher {
public:
    static ErrorOr<NonnullOwnPtr<TimeZoneWatcherImpl>> create()
    {
        return adopt_own(*new TimeZoneWatcherImpl());
    }

    virtual ~TimeZoneWatcherImpl() override
    {
        CFNotificationCenterRemoveObserver(
            CFNotificationCenterGetLocalCenter(),
            this,
            kCFTimeZoneSystemTimeZoneDidChangeNotification,
            nullptr);
    }

private:
    explicit TimeZoneWatcherImpl()
    {
        CFNotificationCenterAddObserver(
            CFNotificationCenterGetLocalCenter(),
            this,
            time_zone_changed,
            kCFTimeZoneSystemTimeZoneDidChangeNotification,
            nullptr,
            CFNotificationSuspensionBehaviorDeliverImmediately);
    }

    static void time_zone_changed(CFNotificationCenterRef, void* observer, CFStringRef, void const*, CFDictionaryRef)
    {
        auto const& time_zone_watcher = *reinterpret_cast<TimeZoneWatcherImpl*>(observer);

        if (time_zone_watcher.on_time_zone_changed)
            time_zone_watcher.on_time_zone_changed();
    }
};

ErrorOr<NonnullOwnPtr<TimeZoneWatcher>> TimeZoneWatcher::create()
{
    return TimeZoneWatcherImpl::create();
}

}
