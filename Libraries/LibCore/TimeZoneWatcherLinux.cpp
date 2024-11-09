/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/TimeZoneWatcher.h>

#if !defined(AK_OS_LINUX)
static_assert(false, "This file must only be used for Linux");
#endif

namespace Core {

static constexpr auto time_zone_files = Array {
    "/etc/localtime"sv,
    "/etc/timezone"sv,
    "/etc/TZ"sv,
};

static constexpr auto time_zone_mask = FileWatcherEvent::Type::ContentModified
    | FileWatcherEvent::Type::Deleted
    | FileWatcherEvent::Type::DoNotFollowLink;

class TimeZoneWatcherImpl final : public TimeZoneWatcher {
public:
    static ErrorOr<NonnullOwnPtr<TimeZoneWatcherImpl>> create()
    {
        auto file_watcher = TRY(FileWatcher::create());

        for (auto time_zone_file : time_zone_files) {
            if (auto result = file_watcher->add_watch(time_zone_file, time_zone_mask); !result.is_error())
                break;
        }

        return adopt_own(*new TimeZoneWatcherImpl(move(file_watcher)));
    }

private:
    explicit TimeZoneWatcherImpl(NonnullRefPtr<FileWatcher> file_watcher)
        : m_file_watcher(move(file_watcher))
    {
        m_file_watcher->on_change = [this](Core::FileWatcherEvent const& event) {
            if (on_time_zone_changed)
                on_time_zone_changed();

            if (has_flag(event.type, FileWatcherEvent::Type::Deleted))
                (void)m_file_watcher->add_watch(event.event_path, time_zone_mask);
        };
    }

    NonnullRefPtr<FileWatcher> m_file_watcher;
};

ErrorOr<NonnullOwnPtr<TimeZoneWatcher>> TimeZoneWatcher::create()
{
    return TimeZoneWatcherImpl::create();
}

}
