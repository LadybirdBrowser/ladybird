/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <UI/Qt/DefaultBrowserMacOS.h>

#import <Cocoa/Cocoa.h>
#import <CoreServices/CoreServices.h>
#import <UI/Qt/Conversions.h>

namespace Ladybird {

using WebView::DefaultBrowserStatus;
using WebView::MakeDefaultBrowserResult;

namespace Detail {

// The system answers on an arbitrary queue, possibly after the browser has quit. Only this atomically counted link
// crosses threads, and only the main thread follows it.
struct DefaultBrowserMacOSLink : public AtomicRefCounted<DefaultBrowserMacOSLink> {
    DefaultBrowserMacOS* default_browser { nullptr };
};

}

static bool opens_urls_with_scheme(NSString* scheme)
{
    auto* url = [NSURL URLWithString:[scheme stringByAppendingString:@"://"]];
    auto* handler_url = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:url];
    if (handler_url == nil)
        return false;

    auto* handler_identifier = [[NSBundle bundleWithURL:handler_url] bundleIdentifier];
    auto* own_identifier = [[NSBundle mainBundle] bundleIdentifier];
    if (handler_identifier == nil)
        return false;

    return [handler_identifier caseInsensitiveCompare:own_identifier] == NSOrderedSame;
}

// The system may ask the user before it changes the handler of a scheme. Asking for one scheme at a time, and stopping
// at the first error, keeps a user who turns it down from being asked again for the next scheme.
static void make_default_for_schemes(NSArray<NSString*>* schemes, NSUInteger index, void (^on_complete)(NSError*))
{
    if (index == schemes.count) {
        on_complete(nil);
        return;
    }

    [[NSWorkspace sharedWorkspace] setDefaultApplicationAtURL:[[NSBundle mainBundle] bundleURL]
                                         toOpenURLsWithScheme:schemes[index]
                                            completionHandler:^(NSError* error) {
                                                if (error != nil) {
                                                    on_complete(error);
                                                    return;
                                                }
                                                make_default_for_schemes(schemes, index + 1, on_complete);
                                            }];
}

// The system wraps whatever went wrong in an error about a file that could not be opened, so what it says of the
// cause is beneath that one.
static NSError* cause_of(NSError* error)
{
    while (auto* underlying_error = error.underlyingErrors.firstObject)
        error = underlying_error;
    return error;
}

static bool is_user_cancellation(NSError* error)
{
    if ([error.domain isEqualToString:NSCocoaErrorDomain])
        return error.code == NSUserCancelledError;
    if ([error.domain isEqualToString:NSOSStatusErrorDomain])
        return error.code == userCanceledErr;
    return false;
}

static MakeDefaultBrowserResult result_from_error(NSError* error)
{
    if (error == nil)
        return {};

    auto* cause = cause_of(error);
    if (is_user_cancellation(cause))
        return {};

    warnln("Unable to make Ladybird the default browser: {}", ns_string_to_string(error.description));
    return { .failed = true, .system_message = ns_string_to_string(cause.localizedDescription) };
}

DefaultBrowserMacOS::DefaultBrowserMacOS()
    : m_link(adopt_ref(*new Detail::DefaultBrowserMacOSLink))
{
    m_link->default_browser = this;
}

DefaultBrowserMacOS::~DefaultBrowserMacOS()
{
    m_link->default_browser = nullptr;
}

void DefaultBrowserMacOS::refresh()
{
    // The system knows applications by their bundles, and a browser run from outside of one has none.
    if ([[NSBundle mainBundle] bundleIdentifier] == nil) {
        did_read_status(DefaultBrowserStatus::Unavailable);
        return;
    }

    auto is_default = opens_urls_with_scheme(@"http") && opens_urls_with_scheme(@"https");
    did_read_status(is_default ? DefaultBrowserStatus::Default : DefaultBrowserStatus::NotDefault);
}

void DefaultBrowserMacOS::make_default(Function<void(MakeDefaultBrowserResult const&)> on_complete)
{
    // Requests made while the system is asking share its answer.
    m_pending_requests.append(move(on_complete));
    if (m_pending_requests.size() > 1)
        return;

    RefPtr<Detail::DefaultBrowserMacOSLink> link = m_link;
    RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();
    make_default_for_schemes(@[ @"http", @"https" ], 0, ^(NSError* error) {
        auto strong_event_loop = event_loop->take();
        if (!strong_event_loop)
            return;
        strong_event_loop->deferred_invoke([link, error] {
            if (link->default_browser)
                link->default_browser->system_did_answer(result_from_error(error));
        });
    });
}

void DefaultBrowserMacOS::system_did_answer(MakeDefaultBrowserResult const& result)
{
    refresh();

    auto pending_requests = move(m_pending_requests);
    for (auto& on_complete : pending_requests)
        on_complete(result);
}

}
