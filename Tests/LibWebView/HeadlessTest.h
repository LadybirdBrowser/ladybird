/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>

namespace Test {

class HeadlessApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(HeadlessApplication)

public:
    explicit HeadlessApplication(Optional<ByteString> ladybird_binary_path);

private:
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual Core::EventLoop& create_platform_event_loop() override { return Core::EventLoop::current(); }
    virtual bool should_coordinate_browser_process() const override { return false; }
};

class HeadlessTestEnvironment {
public:
    explicit HeadlessTestEnvironment(StringView name, unsigned timeout_seconds = 60);
    ~HeadlessTestEnvironment();

    HeadlessTestEnvironment(HeadlessTestEnvironment const&) = delete;
    HeadlessTestEnvironment& operator=(HeadlessTestEnvironment const&) = delete;

    ByteString const& directory() const { return m_directory; }

    template<typename ApplicationType = HeadlessApplication>
    ErrorOr<NonnullOwnPtr<ApplicationType>> create_application(ReadonlySpan<StringView> extra_arguments = {})
    {
        // The application keeps the arguments it is created with.
        m_arguments.shrink(1);
        m_arguments.append(extra_arguments.data(), extra_arguments.size());
        return ApplicationType::create({ .strings = m_arguments }, ladybird_binary_path());
    }

private:
    static Optional<ByteString> ladybird_binary_path();

    ByteString m_directory;
    Vector<StringView, 2> m_arguments { "TestLibWebView"sv };
    Core::EventLoop m_event_loop;
};

}
