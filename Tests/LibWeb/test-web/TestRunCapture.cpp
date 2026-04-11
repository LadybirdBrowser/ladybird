/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestRunCapture.h"
#include "Application.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <AK/LexicalPath.h>
#include <LibCore/System.h>

namespace TestWeb {

static void setup_capture_notifier(RefPtr<Core::Notifier>& notifier, int fd, bool drain_available, Function<void(StringView)> on_output);
static bool drain_capture_output(int fd, bool drain_available, Function<void(StringView)> const& on_output);

TestRunCapture::ViewOutputCapture* TestRunCapture::output_capture_for_view(TestWebView const& view)
{
    auto capture = m_test_output_captures.get(&view);
    if (!capture.has_value())
        return nullptr;
    return capture.value();
}

void TestRunCapture::setup_output_capture_for_view(TestWebView& view, ViewOutputCapture& view_capture)
{
    auto process = Application::the().find_process(view.web_content_pid());
    if (!process.has_value())
        return;

    auto& output_capture = process->output_capture();
    if (!output_capture.stdout_file && !output_capture.stderr_file)
        return;

    if (output_capture.stdout_file) {
        setup_capture_notifier(view_capture.stdout_notifier, output_capture.stdout_file->fd(), false, [&view_capture](StringView message) {
            if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                (void)Core::System::write(STDOUT_FILENO, message.bytes());
            view_capture.output.write(message);
        });
    }

    if (output_capture.stderr_file) {
        setup_capture_notifier(view_capture.stderr_notifier, output_capture.stderr_file->fd(), false, [&view_capture](StringView message) {
            if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                (void)Core::System::write(STDERR_FILENO, message.bytes());
            view_capture.output.write(message);
        });
    }
}

void TestRunCapture::begin_test_output_capture(TestWebView& view, Test const& test)
{
    m_test_output_captures.remove(&view);

    auto view_capture = make<ViewOutputCapture>();
    ByteString output_path = ByteString::formatted("{}.logs.html", LexicalPath::join(Application::the().results_directory, test.safe_relative_path).string());
    view_capture->output = CaptureFile { move(output_path) };

    setup_output_capture_for_view(view, *view_capture);
    m_test_output_captures.set(&view, move(view_capture));
}

void TestRunCapture::rebind_test_output_capture(TestWebView& view)
{
    auto* capture = output_capture_for_view(view);
    if (!capture)
        return;

    setup_output_capture_for_view(view, *capture);
}

void TestRunCapture::write_test_output(TestWebView const& view)
{
    auto* capture = output_capture_for_view(view);
    if (!capture)
        return;

    (void)capture->output.transfer_to_output_file();
    m_test_output_captures.remove(&view);
}

static void setup_capture_notifier(RefPtr<Core::Notifier>& notifier, int fd, bool drain_available, Function<void(StringView)> on_output)
{
    notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    auto* notifier_slot = &notifier;
    notifier->on_activation = [fd, drain_available, on_output = move(on_output), notifier_slot]() mutable {
        bool const reached_eof = drain_capture_output(fd, drain_available, on_output);
        if (reached_eof && *notifier_slot)
            (*notifier_slot)->set_enabled(false);
    };
}

static bool drain_capture_output(int fd, bool drain_available, Function<void(StringView)> const& on_output)
{
    for (;;) {
        char buffer[4096];
        auto nread = read(fd, buffer, sizeof(buffer));
        if (nread > 0) {
            on_output(StringView { buffer, static_cast<size_t>(nread) });
            if (!drain_available)
                return false;
            continue;
        }

        return nread == 0;
    }
}

}
