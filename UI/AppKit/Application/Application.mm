/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Application/EventLoopImplementationMacOS.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <Utilities/Conversions.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

namespace Ladybird {

Application::Application() = default;

void Application::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(m_file_scheme_urls_have_tuple_origins, "Treat file:// URLs as having tuple origins", "tuple-file-origins");
}

void Application::create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&)
{
    if (m_file_scheme_urls_have_tuple_origins)
        URL::set_file_scheme_urls_have_tuple_origins();
}

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

Optional<ByteString> Application::ask_user_for_download_folder() const
{
    auto* panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:NO];
    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];
    [panel setMessage:@"Select download directory"];

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
