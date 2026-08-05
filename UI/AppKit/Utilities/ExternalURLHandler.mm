/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>

#import <Cocoa/Cocoa.h>
#import <UI/AppKit/Utilities/Conversions.h>
#import <UI/AppKit/Utilities/ExternalURLHandler.h>

namespace Ladybird {

class ExternalURLLaunchCompletion final : public AtomicRefCounted<ExternalURLLaunchCompletion> {
public:
    explicit ExternalURLLaunchCompletion(WebView::ExternalURLHandler::LaunchCallback callback)
        : m_callback(move(callback))
    {
    }

    void complete(bool success)
    {
        if (!m_callback)
            return;
        auto callback = move(m_callback);
        callback(success);
    }

private:
    WebView::ExternalURLHandler::LaunchCallback m_callback;
};

class AppKitExternalURLHandler final : public WebView::ExternalURLHandler {
public:
    AppKitExternalURLHandler(String application_name, bool is_ladybird, String application_path)
        : WebView::ExternalURLHandler(move(application_name), is_ladybird)
        , m_application_path(move(application_path))
    {
    }

    virtual void launch(URL::URL const& url, LaunchCallback callback) const override
    {
        auto* ns_url = url_to_ns_url(url);
        auto* application_url = [NSURL fileURLWithPath:string_to_ns_string(m_application_path)];
        if (ns_url == nil || application_url == nil) {
            callback(false);
            return;
        }

        auto completion = adopt_ref(*new ExternalURLLaunchCompletion(move(callback)));
        [[NSWorkspace sharedWorkspace] openURLs:@[ ns_url ]
                           withApplicationAtURL:application_url
                                  configuration:[NSWorkspaceOpenConfiguration configuration]
                              completionHandler:^(NSRunningApplication* application, NSError* error) {
                                  auto success = application != nil && error == nil;
                                  dispatch_async(dispatch_get_main_queue(), ^{
                                      completion->complete(success);
                                  });
                              }];
    }

private:
    String m_application_path;
};

void resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback)
{
    auto* ns_url = url_to_ns_url(url);
    if (ns_url == nil) {
        callback(nullptr);
        return;
    }

    auto* application_url = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:ns_url];
    if (application_url == nil) {
        callback(nullptr);
        return;
    }

    auto* application_name = [[NSFileManager defaultManager] displayNameAtPath:[application_url path]];
    if (application_name == nil)
        application_name = [[application_url lastPathComponent] stringByDeletingPathExtension];

    bool is_ladybird = false;
    if (auto* bundle_identifier = [[NSBundle bundleWithURL:application_url] bundleIdentifier]; bundle_identifier != nil) {
        is_ladybird = [bundle_identifier caseInsensitiveCompare:@"org.ladybird.Ladybird"] == NSOrderedSame;
    }

    if (!is_ladybird) {
        auto* handler_bundle_url = [application_url URLByResolvingSymlinksInPath];
        auto* current_bundle_url = [[[NSBundle mainBundle] bundleURL] URLByResolvingSymlinksInPath];
        is_ladybird = [handler_bundle_url isEqual:current_bundle_url];
    }

    auto name = application_name == nil ? "another application"_string : ns_string_to_string(application_name);
    auto application_path = ns_string_to_string([application_url path]);
    callback(adopt_ref(*new AppKitExternalURLHandler(move(name), is_ladybird, move(application_path))));
}

}
