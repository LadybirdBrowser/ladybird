/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibWebView/ProcessReaper.h>
#include <signal.h>
#include <sys/wait.h>

static ByteString process_reaper_path()
{
    // The helpers live in the application bundle next to the test executables.
    auto test_directory = LexicalPath::dirname(MUST(Core::System::current_executable_path()));
    return ByteString::formatted("{}/Ladybird.app/Contents/MacOS/ProcessReaper", test_directory);
}

TEST_CASE(helpers_are_killed_once_the_browser_is_gone)
{
    OwnPtr<WebView::ProcessReaper> process_reaper = MUST(WebView::ProcessReaper::start(process_reaper_path()));
    auto reaper_pid = process_reaper->pid();

    // A helper that ignores the Browser going away, as a compromised one would.
    Vector<ByteString> arguments { "600"sv };
    auto helper = MUST(Core::Process::spawn({ .executable = "/bin/sleep"sv, .arguments = arguments }));
    MUST(process_reaper->watch(helper.pid()));

    // Closing the pipe is what the Browser's exit or death does.
    process_reaper = nullptr;

    int status = 0;
    VERIFY(waitpid(helper.pid(), &status, 0) == helper.pid());
    EXPECT(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);

    VERIFY(waitpid(reaper_pid, &status, 0) == reaper_pid);
    EXPECT(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(watching_fails_without_killing_the_browser_when_the_reaper_is_gone)
{
    auto process_reaper = MUST(WebView::ProcessReaper::start(process_reaper_path()));
    VERIFY(kill(process_reaper->pid(), SIGKILL) == 0);
    int status = 0;
    VERIFY(waitpid(process_reaper->pid(), &status, 0) == process_reaper->pid());

    // Writing to the pipe must not raise SIGPIPE in this process.
    EXPECT(process_reaper->watch(getpid()).is_error());
}
