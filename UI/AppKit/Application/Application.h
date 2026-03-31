/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Application.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

private:
    explicit Application();

    virtual void create_platform_arguments(Core::ArgsParser&) override;
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual NonnullOwnPtr<Core::EventLoop> create_platform_event_loop() override;

    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override;

    virtual Optional<ByteString> ask_user_for_download_path(StringView file) const override;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const override;
    virtual void display_error_dialog(StringView error_message) const override;

    virtual Utf16String clipboard_text() const override;
    virtual Vector<Web::Clipboard::SystemClipboardRepresentation> clipboard_entries() const override;
    virtual void insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation) override;

    virtual void rebuild_bookmarks_menu() const override;
    virtual void update_bookmarks_bar_display(bool) const override;
    virtual Optional<BookmarkID> bookmark_item_id_for_context_menu() const override;
    virtual NonnullRefPtr<BookmarkPromise> display_add_bookmark_dialog() const override;
    virtual NonnullRefPtr<BookmarkPromise> display_edit_bookmark_dialog(WebView::BookmarkItem::Bookmark const& current_bookmark) const override;
    virtual NonnullRefPtr<BookmarkFolderPromise> display_add_bookmark_folder_dialog() const override;
    virtual NonnullRefPtr<BookmarkFolderPromise> display_edit_bookmark_folder_dialog(WebView::BookmarkItem::Folder const& current_folder) const override;

    virtual void on_devtools_enabled() const override;
    virtual void on_devtools_disabled() const override;

    bool m_file_scheme_urls_have_tuple_origins { false };
};

}

@interface Application : NSApplication
@end
