/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestWebView.h"

#include "Application.h"
#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>

namespace TestWeb {

NonnullOwnPtr<TestWebView> TestWebView::create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
{
    auto view = adopt_own(*new TestWebView(move(theme), window_size));
    view->initialize_client(CreateNewClient::Yes);

    return view;
}

TestWebView::TestWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size)
    : WebView::HeadlessWebView(move(theme), viewport_size)
    , m_test_promise(TestPromise::construct())
{
    load_wpt_resource_map();

    // test-web loads tests with the file:// scheme. WPT has root-relative URLs like
    // "/webaudio/resources/..." then resolve to file:///webaudio/resources/..., which
    // is not within the repository. Use Tests/LibWeb/WptResourceMap.json to remap these
    // file URLs into the in-tree imported WPT files.
    on_request_file = [this](ByteString const& path, i32 request_id) {
        ByteString mapped_path = path;

        if (auto url = URL::create_with_file_scheme(path); url.has_value()) {
            if (auto substituted = map_file_path_from_url(*url); substituted.has_value())
                mapped_path = substituted.release_value();
        }
        auto file = Core::File::open(mapped_path, Core::File::OpenMode::Read);
        if (file.is_error()) {
            client().async_handle_file_return(page_id(), file.error().code(), {}, request_id);
        } else {
            client().async_handle_file_return(page_id(), 0, IPC::File::adopt_file(file.release_value()), request_id);
        }
    };
}

static String normalize_url(URL::URL const& url)
{
    auto normalized = url;
    normalized.set_query({});
    normalized.set_fragment({});
    return normalized.serialize();
}

void TestWebView::load_wpt_resource_map()
{
    auto json_path = LexicalPath::join(Application::the().test_root_path, "WptResourceMap.json"sv).string();

    auto file_or_error = Core::File::open(json_path, Core::File::OpenMode::Read);
    if (file_or_error.is_error()) {
        if (file_or_error.error().code() == ENOENT)
            return;
        warnln("test-web: Unable to open WPT resource map '{}': {}", json_path, file_or_error.error());
        return;
    }

    auto content_or_error = file_or_error.value()->read_until_eof();
    if (content_or_error.is_error()) {
        warnln("test-web: Unable to read WPT resource map '{}': {}", json_path, content_or_error.error());
        return;
    }

    auto json_or_error = JsonValue::from_string(content_or_error.value());
    if (json_or_error.is_error()) {
        warnln("test-web: Unable to parse WPT resource map '{}': {}", json_path, json_or_error.error());
        return;
    }

    auto json = json_or_error.release_value();
    if (!json.is_object()) {
        warnln("test-web: WPT resource map must be a JSON object");
        return;
    }

    auto const& root = json.as_object();

    // Simple prefix map: { "file:///webaudio/resources/": "Text/input/wpt-import/webaudio/resources/" }
    root.for_each_member([&](auto const& url_prefix, auto const& file_prefix_value) {
        if (!file_prefix_value.is_string())
            return;

        ByteString file_prefix = file_prefix_value.as_string().to_byte_string();
        if (!LexicalPath { file_prefix }.is_absolute())
            file_prefix = LexicalPath::join(Application::the().test_root_path, file_prefix).string();

        m_wpt_file_substitutions.set(url_prefix, move(file_prefix));
    });
}

Optional<ByteString> TestWebView::map_file_path_from_url(URL::URL const& url) const
{
    auto normalized = normalize_url(url);
    Optional<String> best_prefix;
    Optional<ByteString> best_file_prefix;

    for (auto const& it : m_wpt_file_substitutions) {
        if (!normalized.starts_with_bytes(it.key.bytes_as_string_view()))
            continue;
        if (!best_prefix.has_value() || it.key.byte_count() > best_prefix->byte_count()) {
            best_prefix = it.key;
            best_file_prefix = it.value;
        }
    }
    if (!best_prefix.has_value())
        return {};

    String suffix = MUST(normalized.substring_from_byte_offset(best_prefix->byte_count()));
    return LexicalPath::join(*best_file_prefix, suffix.to_byte_string()).string();
}

void TestWebView::clear_content_filters()
{
    client().async_set_content_filters(m_client_state.page_index, {});
}

pid_t TestWebView::web_content_pid() const
{
    return client().pid();
}

NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap const>>> TestWebView::take_screenshot()
{
    VERIFY(!m_pending_screenshot);

    m_pending_screenshot = Core::Promise<RefPtr<Gfx::Bitmap const>>::construct();
    client().async_take_document_screenshot(0);

    return *m_pending_screenshot;
}

void TestWebView::did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot)
{
    if (!m_pending_screenshot) {
        static bool warned_about_stray_screenshot = false;
        if (!warned_about_stray_screenshot) {
            warned_about_stray_screenshot = true;
            warnln("Ignoring screenshot response with no pending request");
        }
        return;
    }

    auto pending_screenshot = move(m_pending_screenshot);
    pending_screenshot->resolve(screenshot.bitmap());
}

void TestWebView::on_test_complete(TestCompletion completion)
{
    m_pending_screenshot.clear();
    m_pending_dialog = Web::Page::PendingDialog::None;
    m_pending_prompt_text.clear();

    m_test_promise->resolve(completion);
}

}
