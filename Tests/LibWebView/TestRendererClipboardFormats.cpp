/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/AnyOf.h>
#include <AK/LexicalPath.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace {

// Outside test mode and without the internals object, a renderer's clipboard writes are filtered.
class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
        web_content_options.expose_internals_object = WebView::ExposeInternalsObject::No;
    }
};

}

TEST_CASE(renderer_clipboard_formats)
{
    Test::HeadlessTestEnvironment environment { "TestRendererClipboardFormats"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    // The message goes straight to the UI process's handler, so a stub client is enough.
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto& stub = static_cast<WebContentClientStub&>(view->page().client());

    auto has_format = [&](StringView name) {
        return any_of(app->clipboard_item().system_clipboard_representations, [&](auto const& representation) {
            return representation.name == name;
        });
    };

    auto write = [&](Vector<Web::Clipboard::SystemClipboardRepresentation> representations) {
        view->page().give_user_activation();
        stub.did_insert_clipboard_item(view->page().id(), { move(representations) }, "unspecified"_string, {});
    };

    write({
        { "text/uri-list"_string, ByteString { "file:///etc/hosts"sv } },
        { "text/plain"_string, ByteString { "text"sv } },
        { "text/html"_string, ByteString { "<b>html</b>"sv } },
        { "image/png"_string, ByteString { "png"sv } },
    });
    EXPECT(!has_format("text/uri-list"sv));
    EXPECT(has_format("text/plain"sv));
    EXPECT(has_format("text/html"sv));
    EXPECT(has_format("image/png"sv));

    // A write with no format a page writes leaves the clipboard as it was.
    write({ { "text/uri-list"_string, ByteString { "file:///etc/hosts"sv } } });
    EXPECT(!has_format("text/uri-list"sv));
    EXPECT(has_format("text/plain"sv));

    // A file is dropped, even under a format a page writes.
    auto file = [] {
        return Web::HTML::SelectedFile { "file.txt"_utf16, MUST(ByteBuffer::copy("file"sv.bytes())) };
    };
    write({
        { "text/plain"_string, file() },
        { "text/html"_string, ByteString { "<b>html</b>"sv } },
    });
    EXPECT(!has_format("text/plain"sv));
    EXPECT(has_format("text/html"sv));

    // A write left with nothing leaves the clipboard as it was.
    write({ { "text/plain"_string, file() } });
    EXPECT(!has_format("text/plain"sv));
    EXPECT(has_format("text/html"sv));
}
