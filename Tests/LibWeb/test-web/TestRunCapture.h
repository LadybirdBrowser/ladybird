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
#include <AK/StringView.h>
#include <LibCore/Notifier.h>

namespace TestWeb {

class TestWebView;
struct Test;

class TestRunCapture {
public:
    TestRunCapture() = default;

    void begin_test_output_capture(TestWebView&, Test const&);
    void rebind_test_output_capture(TestWebView&);
    void write_test_output(TestWebView const&);

    TestRunCapture(TestRunCapture const&) = delete;
    TestRunCapture& operator=(TestRunCapture const&) = delete;

private:
    struct ViewOutputCapture {
        CaptureFile output;
        RefPtr<Core::Notifier> stdout_notifier;
        RefPtr<Core::Notifier> stderr_notifier;
    };

    ViewOutputCapture* output_capture_for_view(TestWebView const&);
    void setup_output_capture_for_view(TestWebView&, ViewOutputCapture&);

    HashMap<TestWebView const*, NonnullOwnPtr<ViewOutputCapture>> m_test_output_captures;
};

}
