/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Application/EventLoopImplementationMacOS.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>
#include <Utilities/Conversions.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Interface/BookmarksBar.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

namespace Ladybird {

Application::Application() = default;

NonnullOwnPtr<Core::EventLoop> Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        Core::EventLoopManager::install(*new EventLoopManagerMacOS);
        [::Application sharedApplication];
    }

    return WebView::Application::create_platform_event_loop();
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    if (auto* tab = [delegate activeTab])
        return [[tab web_view] view];
    return {};
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    auto* controller = [delegate createNewTab:activate_tab fromTab:[delegate activeTab]];
    auto* tab = (Tab*)[controller window];

    return [[tab web_view] view];
}

Optional<ByteString> Application::ask_user_for_download_path(StringView file) const
{
    auto* panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:Ladybird::string_to_ns_string(file)];
    [panel setTitle:@"Select save location"];

    if ([panel runModal] != NSModalResponseOK)
        return {};

    return Ladybird::ns_string_to_byte_string([[panel URL] path]);
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    auto message = MUST(String::formatted("{} saved to: {}", download_name, path));

    auto* dialog = [[NSAlert alloc] init];
    [dialog setMessageText:Ladybird::string_to_ns_string(message)];
    [[dialog addButtonWithTitle:@"OK"] setTag:NSModalResponseOK];
    [[dialog addButtonWithTitle:@"Open folder"] setTag:NSModalResponseContinue];

    __block auto* ns_path = Ladybird::string_to_ns_string(path.string());

    [dialog beginSheetModalForWindow:[delegate activeTab]
                   completionHandler:^(NSModalResponse response) {
                       if (response == NSModalResponseContinue) {
                           [[NSWorkspace sharedWorkspace] selectFile:ns_path inFileViewerRootedAtPath:@""];
                       }
                   }];
}

void Application::display_error_dialog(StringView error_message) const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    auto* dialog = [[NSAlert alloc] init];
    [dialog setMessageText:Ladybird::string_to_ns_string(error_message)];

    [dialog beginSheetModalForWindow:[delegate activeTab]
                   completionHandler:nil];
}

Utf16String Application::clipboard_text() const
{
    auto* paste_board = [NSPasteboard generalPasteboard];

    if (auto* contents = [paste_board stringForType:NSPasteboardTypeString])
        return Ladybird::ns_string_to_utf16_string(contents);
    return {};
}

Vector<Web::Clipboard::SystemClipboardRepresentation> Application::clipboard_entries() const
{
    Vector<Web::Clipboard::SystemClipboardRepresentation> representations;
    auto* paste_board = [NSPasteboard generalPasteboard];

    for (NSPasteboardType type : [paste_board types]) {
        String mime_type;

        if (type == NSPasteboardTypeString)
            mime_type = "text/plain"_string;
        else if (type == NSPasteboardTypeHTML)
            mime_type = "text/html"_string;
        else if (type == NSPasteboardTypePNG)
            mime_type = "image/png"_string;
        else
            continue;

        auto data = Ladybird::ns_data_to_string([paste_board dataForType:type]);
        representations.empend(move(data), move(mime_type));
    }

    return representations;
}

void Application::insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation entry)
{
    NSPasteboardType pasteboard_type = nil;

    // https://w3c.github.io/clipboard-apis/#os-specific-well-known-format
    if (entry.mime_type == "text/plain"sv)
        pasteboard_type = NSPasteboardTypeString;
    else if (entry.mime_type == "text/html"sv)
        pasteboard_type = NSPasteboardTypeHTML;
    else if (entry.mime_type == "image/png"sv)
        pasteboard_type = NSPasteboardTypePNG;
    else
        return;

    auto* paste_board = [NSPasteboard generalPasteboard];
    [paste_board clearContents];

    [paste_board setData:Ladybird::string_to_ns_data(entry.data)
                 forType:pasteboard_type];
}

void Application::rebuild_bookmarks_menu() const
{
    ApplicationDelegate* delegate = [NSApp delegate];
    [delegate rebuildBookmarksMenu];
}

void Application::update_bookmarks_bar_display(bool show_bookmarks_bar) const
{
    ApplicationDelegate* delegate = [NSApp delegate];
    [delegate updateBookmarksBarDisplay:show_bookmarks_bar];
}

void Application::show_bookmark_context_menu(Gfx::IntPoint content_position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id)
{
    ApplicationDelegate* delegate = [NSApp delegate];

    if (auto* tab = [delegate activeTab]) {
        [[tab bookmarksBar] showContextMenu:content_position
                                       view:[tab web_view]
                               bookmarkItem:item
                             targetFolderID:target_folder_id];
    }
}

Optional<Application::BookmarkID> Application::bookmark_item_id_for_context_menu() const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    if (auto* tab = [delegate activeTab]) {
        auto* bookmarks_bar = [tab bookmarksBar];

        return Application::BookmarkID {
            .id = Ladybird::ns_string_to_string([bookmarks_bar selected_bookmark_menu_item_id]),
            .target_folder_id = [bookmarks_bar selected_bookmark_menu_target_folder_id]
                ? Optional<String> { Ladybird::ns_string_to_string([bookmarks_bar selected_bookmark_menu_target_folder_id]) }
                : Optional<String> {},
        };
    }

    return {};
}

static constexpr CGFloat BOOKMARK_LABEL_WIDTH = 40;
static constexpr CGFloat BOOKMARK_TEXT_WIDTH = 300;
static constexpr CGFloat BOOKMARK_SPACING = 8;

static NSTextField* create_bookmark_dialog_text_field(Optional<String const&> text)
{
    auto* text_field = [[NSTextField alloc] init];
    [[text_field cell] setScrollable:YES];
    [[text_field cell] setWraps:NO];
    [[text_field widthAnchor] constraintEqualToConstant:BOOKMARK_TEXT_WIDTH].active = YES;

    if (text.has_value())
        [text_field setStringValue:Ladybird::string_to_ns_string(*text)];

    return text_field;
}

static NSView* create_bookmark_dialog_row(NSString* label_text, NSTextField* text_field)
{
    auto* row = [[NSStackView alloc] init];
    [row setAlignment:NSLayoutAttributeCenterY];
    [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [row setSpacing:BOOKMARK_SPACING];

    auto* label = [NSTextField labelWithString:label_text];
    [label setAlignment:NSTextAlignmentRight];
    [[label widthAnchor] constraintEqualToConstant:BOOKMARK_LABEL_WIDTH].active = YES;

    [row addArrangedSubview:label];
    [row addArrangedSubview:text_field];

    auto size = [row fittingSize];
    [row setFrame:NSMakeRect(0, 0, size.width, size.height)];
    return row;
}

static NSAlert* create_bookmark_dialog(NSString* title, NSView* first_responder, NSArray<NSView*>* rows)
{
    auto* container = [[NSStackView alloc] init];
    [container setAlignment:NSLayoutAttributeLeading];
    [container setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [container setSpacing:BOOKMARK_SPACING];

    for (NSView* row in rows)
        [container addArrangedSubview:row];

    auto size = [container fittingSize];
    [container setFrame:NSMakeRect(0, 0, size.width, size.height)];

    auto* dialog = [[NSAlert alloc] init];
    [dialog setAccessoryView:container];
    [dialog setMessageText:title];
    [[dialog addButtonWithTitle:@"OK"] setTag:NSModalResponseOK];
    [[dialog addButtonWithTitle:@"Cancel"] setTag:NSModalResponseCancel];
    [[dialog window] setInitialFirstResponder:first_responder];

    return dialog;
}

template<typename PromiseType>
static NonnullRefPtr<PromiseType> display_add_or_edit_bookmark_dialog(
    Tab* parent,
    NSString* title,
    Optional<URL::URL const&> current_url,
    Optional<String const&> current_title)
{
    auto promise = PromiseType::construct();

    auto* url_field = create_bookmark_dialog_text_field(current_url.map([](auto const& url) { return url.serialize(); }));
    auto* title_field = create_bookmark_dialog_text_field(current_title);

    auto* dialog = create_bookmark_dialog(title, url_field, @[
        create_bookmark_dialog_row(@"URL:", url_field),
        create_bookmark_dialog_row(@"Title:", title_field),
    ]);

    [dialog beginSheetModalForWindow:parent
                   completionHandler:^(NSModalResponse response) {
                       if (response != NSModalResponseOK) {
                           promise->reject(Error::from_errno(ECANCELED));
                           return;
                       }

                       auto url = WebView::sanitize_url(Ladybird::ns_string_to_string([url_field stringValue]));
                       if (!url.has_value()) {
                           promise->reject(Error::from_errno(EINVAL));
                           return;
                       }

                       Optional<String> bookmark_title;
                       if (auto text = Ladybird::ns_string_to_string([title_field stringValue]); !text.is_empty())
                           bookmark_title = move(text);

                       promise->resolve(WebView::BookmarkItem::Bookmark {
                           .url = url.release_value(),
                           .title = move(bookmark_title),
                           .favicon_base64_png = {},
                       });
                   }];

    return promise;
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_add_bookmark_dialog() const
{
    ApplicationDelegate* delegate = [NSApp delegate];

    Optional<URL::URL> current_url;
    Optional<String> current_title;

    if (auto view = active_web_view(); view.has_value()) {
        current_url = view->url();
        current_title = view->title().to_utf8();
    }

    return display_add_or_edit_bookmark_dialog<BookmarkPromise>([delegate activeTab], @"Add Bookmark", current_url, current_title);
}

NonnullRefPtr<Application::BookmarkPromise> Application::display_edit_bookmark_dialog(WebView::BookmarkItem::Bookmark const& current_bookmark) const
{
    ApplicationDelegate* delegate = [NSApp delegate];
    return display_add_or_edit_bookmark_dialog<BookmarkPromise>([delegate activeTab], @"Edit Bookmark", current_bookmark.url, current_bookmark.title);
}

template<typename PromiseType>
static NonnullRefPtr<PromiseType> display_add_or_edit_bookmark_folder_dialog(
    Tab* parent,
    NSString* title,
    Optional<String const&> current_title)
{
    auto promise = PromiseType::construct();

    auto* title_field = create_bookmark_dialog_text_field(current_title);

    auto* dialog = create_bookmark_dialog(title, title_field, @[
        create_bookmark_dialog_row(@"Title:", title_field),
    ]);

    [dialog beginSheetModalForWindow:parent
                   completionHandler:^(NSModalResponse response) {
                       if (response != NSModalResponseOK) {
                           promise->reject(Error::from_errno(ECANCELED));
                           return;
                       }

                       Optional<String> folder_title;
                       if (auto text = Ladybird::ns_string_to_string([title_field stringValue]); !text.is_empty())
                           folder_title = move(text);

                       promise->resolve(WebView::BookmarkItem::Folder {
                           .title = move(folder_title),
                           .children = {},
                       });
                   }];

    return promise;
}

NonnullRefPtr<Application::BookmarkFolderPromise> Application::display_add_bookmark_folder_dialog() const
{
    ApplicationDelegate* delegate = [NSApp delegate];
    return display_add_or_edit_bookmark_folder_dialog<BookmarkFolderPromise>([delegate activeTab], @"Add Folder", {});
}

NonnullRefPtr<Application::BookmarkFolderPromise> Application::display_edit_bookmark_folder_dialog(WebView::BookmarkItem::Folder const& current_folder) const
{
    ApplicationDelegate* delegate = [NSApp delegate];
    return display_add_or_edit_bookmark_folder_dialog<BookmarkFolderPromise>([delegate activeTab], @"Edit Folder", current_folder.title);
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();

    ApplicationDelegate* delegate = [NSApp delegate];
    [delegate onDevtoolsEnabled];
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();

    ApplicationDelegate* delegate = [NSApp delegate];
    [delegate onDevtoolsDisabled];
}

}

@interface Application ()
@end

@implementation Application

#pragma mark - NSApplication

- (void)terminate:(id)sender
{
    Core::EventLoop::current().quit(0);
}

- (void)sendEvent:(NSEvent*)event
{
    if ([event type] == NSEventTypeApplicationDefined) {
        Core::ThreadEventQueue::current().process();
    } else {
        [super sendEvent:event];
    }
}

@end
