/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Types.h>

#include <LibCore/Forward.h>

namespace TestWeb {

class TestWebView;
struct Test;
enum class TestResult;

struct SgrStripperState {
    enum class Mode : u8 {
        Normal,
        SawEsc,
        InCsi,
    };

    Mode mode { Mode::Normal };
    size_t pending_length { 0 };
    Array<u8, 64> pending_bytes {};

    void reset()
    {
        mode = Mode::Normal;
        pending_length = 0;
    }
};

struct ViewOutputCapture {

    RefPtr<Core::Notifier> stdout_notifier;
    RefPtr<Core::Notifier> stderr_notifier;
    int stdout_fd { -1 };
    int stderr_fd { -1 };
    bool tee_to_terminal { false };

    OwnPtr<Core::File> stdout_temp_file;
    OwnPtr<Core::File> stderr_temp_file;
    ByteString stdout_temp_path;
    ByteString stderr_temp_path;

    SgrStripperState stdout_sgr_stripper;
    SgrStripperState stderr_sgr_stripper;
};

void maybe_attach_on_fail_fast_timeout(pid_t);

void append_timeout_diagnostics_to_stderr(StringBuilder&, TestWebView&, Test const&, size_t view_id);
void append_timeout_backtraces_to_stderr(StringBuilder&, TestWebView&, Test const&, size_t view_id);

ErrorOr<ByteString> create_temp_file_path(StringView directory, StringView prefix);

ViewOutputCapture* ensure_output_capture_for_view(TestWebView&);
ViewOutputCapture* output_capture_for_view(TestWebView&);
void remove_output_capture_for_view(TestWebView&);

ErrorOr<void> begin_output_capture_for_test(TestWebView&, Test const&);
ErrorOr<void> finalize_output_for_test(Test const&, TestResult, ViewOutputCapture&, StringView base_path);
ErrorOr<void> append_to_stderr_capture(ViewOutputCapture&, StringView);

}
