/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CaptureFile.h"

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibWebView/Process.h>

namespace TestWeb {

class TestWebView;
struct Test;

class TestRunCapture {
public:
    TestRunCapture();
    ~TestRunCapture();

    void begin_test_output_capture(TestWebView&, Test const&);
    void rebind_test_output_capture(TestWebView&);
    void write_test_output(TestWebView const&);
    bool write_helper_process_output();

    TestRunCapture(TestRunCapture const&) = delete;
    TestRunCapture& operator=(TestRunCapture const&) = delete;

private:
    struct ViewOutputCapture {
        CaptureFile output;
        RefPtr<Core::Notifier> stdout_notifier;
        RefPtr<Core::Notifier> stderr_notifier;
    };

    struct HelperOutputSource {
        WebView::ProcessType type;
        pid_t pid { 0 };
    };

    struct HelperOutputCapture {
        WebView::ProcessType type;
        pid_t pid { 0 };
        OwnPtr<Core::File> stdout_reader;
        OwnPtr<Core::File> stderr_reader;
        RefPtr<Core::Notifier> stdout_notifier;
        RefPtr<Core::Notifier> stderr_notifier;
    };

    struct BrowserStderrCapture {
        OwnPtr<Core::File> reader;
        RefPtr<Core::Notifier> notifier;
        Optional<int> original_fd;
    };

    ViewOutputCapture* output_capture_for_view(TestWebView const&);
    void log_helper_message(HelperOutputSource, int tee_fd, StringView);
    void restore_stderr();
    void setup_output_capture_for_helper_process(WebView::Process&);
    void setup_output_capture_for_view(TestWebView&, ViewOutputCapture&);
    void consume_helper_capture(pid_t pid);
    void destroy_view_capture_of(TestWebView const& view);

    Function<void(WebView::Process&&)> m_previous_on_process_exited;
    HashMap<TestWebView const*, NonnullOwnPtr<ViewOutputCapture>> m_test_output_captures;
    HashMap<pid_t, NonnullOwnPtr<HelperOutputCapture>> m_helper_output_captures;
    CaptureFile m_helper_output;
    Optional<HelperOutputSource> m_last_helper_source;
    UnixDateTime m_run_started_at;
    BrowserStderrCapture m_stderr_capture;
};

}
