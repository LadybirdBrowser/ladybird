/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>
#include <fcntl.h>
#include <unistd.h>

TEST_CASE(file_watcher_child_events)
{
    auto event_loop = Core::EventLoop();
    auto maybe_file_watcher = Core::FileWatcher::create();
    EXPECT_NE(maybe_file_watcher.is_error(), true);

    auto file_watcher = maybe_file_watcher.release_value();
    auto watch_result = file_watcher->add_watch("/tmp/",
        Core::FileWatcherEvent::Type::ChildCreated
            | Core::FileWatcherEvent::Type::ChildDeleted);
    EXPECT_NE(watch_result.is_error(), true);

    int event_count = 0;
    file_watcher->on_change = [&](Core::FileWatcherEvent const& event) {
        // Ignore path events under /tmp that can occur for anything else the OS is
        // doing to create/delete files there.
        if (event.event_path != "/tmp/testfile"sv)
            return;

        if (event_count == 0) {
            EXPECT(has_flag(event.type, Core::FileWatcherEvent::Type::ChildCreated));
        } else if (event_count == 1) {
            EXPECT(has_flag(event.type, Core::FileWatcherEvent::Type::ChildDeleted));
            EXPECT(MUST(file_watcher->remove_watch("/tmp/"sv)));

            event_loop.quit(0);
        }

        event_count++;
    };

    auto timer1 = Core::Timer::create_single_shot(500, [&] {
        int rc = creat("/tmp/testfile", 0777);
        EXPECT_NE(rc, -1);
    });
    timer1->start();

    auto timer2 = Core::Timer::create_single_shot(1000, [&] {
        int rc = unlink("/tmp/testfile");
        EXPECT_NE(rc, -1);
    });
    timer2->start();

    auto catchall_timer = Core::Timer::create_single_shot(2000, [&] {
        VERIFY_NOT_REACHED();
    });
    catchall_timer->start();

    event_loop.exec();
}
