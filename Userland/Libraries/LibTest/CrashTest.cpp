/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2019-2020, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Platform.h>
#include <LibTest/CrashTest.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(AK_OS_MACOS) && !defined(AK_OS_EMSCRIPTEN) && !defined(AK_OS_GNU_HURD)
#    include <sys/prctl.h>
#endif

namespace Test {

Crash::Crash(ByteString test_type, Function<Crash::Failure()> crash_function, int crash_signal)
    : m_type(move(test_type))
    , m_crash_function(move(crash_function))
    , m_crash_signal(crash_signal)
{
}

bool Crash::run(RunType run_type)
{
    outln("\x1B[33mTesting\x1B[0m: \"{}\"", m_type);

    if (run_type == RunType::UsingCurrentProcess) {
        return do_report(m_crash_function());
    } else {
        // Run the test in a child process so that we do not crash the crash program :^)
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            VERIFY_NOT_REACHED();
        } else if (pid == 0) {
#if defined(AK_OS_GNU_HURD)
            // When we crash, just kill the program, don't dump core.
            setenv("CRASHSERVER", "/servers/crash-kill", true);
#elif !defined(AK_OS_MACOS) && !defined(AK_OS_EMSCRIPTEN)
            if (prctl(PR_SET_DUMPABLE, 0, 0, 0) < 0)
                perror("prctl(PR_SET_DUMPABLE)");
#endif
            exit((int)m_crash_function());
        }

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return do_report(Failure(WEXITSTATUS(status)));
        }
        if (WIFSIGNALED(status)) {
            int signal = WTERMSIG(status);
            VERIFY(signal > 0);
            return do_report(signal);
        }
        VERIFY_NOT_REACHED();
    }
}

bool Crash::do_report(Report report)
{
    bool pass = false;
    if (m_crash_signal == ANY_SIGNAL) {
        pass = report.has<int>();
    } else if (m_crash_signal == 0) {
        pass = report.has<Failure>() && report.get<Failure>() == Failure::DidNotCrash;
    } else if (m_crash_signal > 0) {
        pass = report.has<int>() && report.get<int>() == m_crash_signal;
    } else {
        VERIFY_NOT_REACHED();
    }

    if (pass)
        out("\x1B[32mPASS\x1B[0m: ");
    else
        out("\x1B[31mFAIL\x1B[0m: ");

    report.visit(
        [&](Failure const& failure) {
            switch (failure) {
            case Failure::DidNotCrash:
                out("Did not crash");
                break;
            case Failure::UnexpectedError:
                out("Unexpected error");
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        },
        [&](int const& signal) {
            out("Terminated with signal {}", signal);
        });

    if (!pass) {
        if (m_crash_signal == ANY_SIGNAL) {
            out(" while expecting any signal");
        } else if (m_crash_signal > 0) {
            out(" while expecting signal {}", m_crash_signal);
        }
    }
    outln();

    return pass;
}

}
