/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef AK_OS_MACOS
constexpr int TIMEOUT_PER_STEP_IN_MS = 350;
#else
constexpr int TIMEOUT_PER_STEP_IN_MS = 75;
#endif

TEST_CASE(file_watcher_child_events)
{
    auto event_loop = Core::EventLoop();
    auto maybe_file_watcher = Core::FileWatcher::create();
    EXPECT_NE(maybe_file_watcher.is_error(), true);

    // Ensure the testfile does not already exist.
    (void)Core::System::unlink("/tmp/testfile"sv);

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

    auto timer1 = Core::Timer::create_single_shot(1 * TIMEOUT_PER_STEP_IN_MS, [&] {
        int rc = creat("/tmp/testfile", 0777);
        EXPECT_NE(rc, -1);
    });
    timer1->start();

    auto timer2 = Core::Timer::create_single_shot(2 * TIMEOUT_PER_STEP_IN_MS, [&] {
        MUST(Core::System::unlink("/tmp/testfile"sv));
    });
    timer2->start();

    auto catchall_timer = Core::Timer::create_single_shot(3 * TIMEOUT_PER_STEP_IN_MS, [&] {
        VERIFY_NOT_REACHED();
    });
    catchall_timer->start();

    event_loop.exec();
}

TEST_CASE(contents_changed)
{
    auto event_loop = Core::EventLoop();

    auto temp_path = MUST(FileSystem::real_path("/tmp"sv));
    auto test_path = LexicalPath::join(temp_path, "testfile"sv);

    auto write_file = [&](auto contents) {
        auto file = MUST(Core::File::open(test_path.string(), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(contents));
    };

    write_file("line1\n"sv);

    auto file_watcher = MUST(Core::FileWatcher::create());
    MUST(file_watcher->add_watch(test_path.string(), Core::FileWatcherEvent::Type::ContentModified));

    int event_count = 0;
    file_watcher->on_change = [&](Core::FileWatcherEvent const& event) {
        EXPECT_EQ(event.event_path, test_path.string());
        EXPECT(has_flag(event.type, Core::FileWatcherEvent::Type::ContentModified));

        if (++event_count == 2) {
            MUST(Core::System::unlink(test_path.string()));
            event_loop.quit(0);
        }
    };

    auto timer1 = Core::Timer::create_single_shot(1 * TIMEOUT_PER_STEP_IN_MS, [&] { write_file("line2\n"sv); });
    timer1->start();

    auto timer2 = Core::Timer::create_single_shot(2 * TIMEOUT_PER_STEP_IN_MS, [&] { write_file("line3\n"sv); });
    timer2->start();

    auto catchall_timer = Core::Timer::create_single_shot(3 * TIMEOUT_PER_STEP_IN_MS, [&] {
        VERIFY_NOT_REACHED();
    });
    catchall_timer->start();

    event_loop.exec();
}

TEST_CASE(symbolic_link)
{
    auto event_loop = Core::EventLoop();

    auto temp_path = MUST(FileSystem::real_path("/tmp"sv));
    auto test_file = LexicalPath::join(temp_path, "testfile"sv);
    auto test_link1 = LexicalPath::join(temp_path, "testlink1"sv);
    auto test_link2 = LexicalPath::join(temp_path, "testlink2"sv);

    (void)MUST(Core::File::open(test_link1.string(), Core::File::OpenMode::ReadWrite));
    (void)MUST(Core::File::open(test_link2.string(), Core::File::OpenMode::ReadWrite));
    MUST(Core::System::symlink(test_link1.string(), test_file.string()));

    auto file_watcher = MUST(Core::FileWatcher::create());
    MUST(file_watcher->add_watch(test_file.string(), Core::FileWatcherEvent::Type::Deleted | Core::FileWatcherEvent::Type::DoNotFollowLink));

    int event_count = 0;
    file_watcher->on_change = [&](Core::FileWatcherEvent const& event) {
        EXPECT_EQ(event.event_path, test_file.string());
        EXPECT(has_flag(event.type, Core::FileWatcherEvent::Type::Deleted));

        MUST(file_watcher->add_watch(test_file.string(), Core::FileWatcherEvent::Type::Deleted | Core::FileWatcherEvent::Type::DoNotFollowLink));

        if (++event_count == 2) {
            MUST(Core::System::unlink(test_file.string()));
            MUST(Core::System::unlink(test_link1.string()));
            MUST(Core::System::unlink(test_link2.string()));
            event_loop.quit(0);
        }
    };

    auto timer1 = Core::Timer::create_single_shot(1 * TIMEOUT_PER_STEP_IN_MS, [&] {
        MUST(Core::System::unlink(test_file.string()));
        MUST(Core::System::symlink(test_link1.string(), test_file.string()));
    });
    timer1->start();

    auto timer2 = Core::Timer::create_single_shot(2 * TIMEOUT_PER_STEP_IN_MS, [&] {
        MUST(Core::System::unlink(test_file.string()));
        MUST(Core::System::symlink(test_link2.string(), test_file.string()));
    });
    timer2->start();

    auto catchall_timer = Core::Timer::create_single_shot(3 * TIMEOUT_PER_STEP_IN_MS, [&] {
        VERIFY_NOT_REACHED();
    });
    catchall_timer->start();

    event_loop.exec();
}
