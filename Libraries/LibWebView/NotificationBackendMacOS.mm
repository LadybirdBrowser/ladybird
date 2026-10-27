/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibWebView/NotificationAttribution.h>
#include <LibWebView/NotificationBackendMacOS.h>

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

@interface LadybirdNotificationCenterDelegate : NSObject <UNUserNotificationCenterDelegate>
- (instancetype)initWithBackend:(WebView::NotificationBackendMacOS*)backend;
- (void)detach;
@end

namespace WebView {

// The bridge holds the center and the delegate as plain pointers, so the header stays C++; these read them back.
static UNUserNotificationCenter* center_of(Detail::NotificationCenterBridge& bridge)
{
    return (__bridge UNUserNotificationCenter*)bridge.center;
}

static LadybirdNotificationCenterDelegate* delegate_of(Detail::NotificationCenterBridge& bridge)
{
    return (__bridge LadybirdNotificationCenterDelegate*)bridge.delegate;
}

// Every callback from the center arrives on a queue of its own; the browser's state is touched on the main thread only.
static void on_main_thread(Function<void()> function)
{
    __block auto held = move(function);
    dispatch_async(dispatch_get_main_queue(), ^{
        held();
    });
}

static NSString* ns_string(String const& string)
{
    return [NSString stringWithUTF8String:string.to_byte_string().characters()];
}

NotificationBackendMacOS::NotificationBackendMacOS()
    : m_bridge(make<Detail::NotificationCenterBridge>())
{
    m_bundled = [[NSBundle mainBundle] bundleIdentifier] != nil;
    if (!m_bundled) {
        dbgln("Notifications: Not running from an application bundle, so there is no notification center to show them in");
        return;
    }
    m_bridge->center = (__bridge_retained void*)[UNUserNotificationCenter currentNotificationCenter];
    m_bridge->delegate = (__bridge_retained void*)[[LadybirdNotificationCenterDelegate alloc] initWithBackend:this];
    center_of(*m_bridge).delegate = delegate_of(*m_bridge);
    m_launch_token = MUST(String::from_utf8(StringView { [[[NSUUID UUID] UUIDString] UTF8String], strlen([[[NSUUID UUID] UUIDString] UTF8String]) }));

    // The center reports a user's dismissal only for a category that asks for it.
    auto* category = [UNNotificationCategory categoryWithIdentifier:@"org.ladybird.Ladybird.web-notification"
                                                            actions:@[]
                                                  intentIdentifiers:@[]
                                                            options:UNNotificationCategoryOptionCustomDismissAction];
    [center_of(*m_bridge) setNotificationCategories:[NSSet setWithObject:category]];
}

NotificationBackendMacOS::~NotificationBackendMacOS()
{
    if (m_bridge->delegate) {
        auto* delegate = (__bridge_transfer LadybirdNotificationCenterDelegate*)m_bridge->delegate;
        auto* center = (__bridge_transfer UNUserNotificationCenter*)m_bridge->center;
        [delegate detach];
        if (center.delegate == delegate)
            center.delegate = nil;
    }
}

void NotificationBackendMacOS::request_authorization_then(Function<void(bool)> then)
{
    switch (m_authorization) {
    case Authorization::Granted:
        then(true);
        return;
    case Authorization::Denied:
        then(false);
        return;
    case Authorization::Pending:
        m_waiting_for_authorization.append(move(then));
        return;
    case Authorization::Unknown:
        break;
    }

    m_authorization = Authorization::Pending;
    m_waiting_for_authorization.append(move(then));
    [center_of(*m_bridge) requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                        completionHandler:^(BOOL granted, NSError* error) {
                                            if (!granted)
                                                dbgln("Notifications: The notification center denied authorization{}{}", error ? ": "sv : ""sv, error ? [[error localizedDescription] UTF8String] : "");
                                            on_main_thread([this, granted = static_cast<bool>(granted)] {
                                                m_authorization = granted ? Authorization::Granted : Authorization::Denied;
                                                for (auto& waiting : exchange(m_waiting_for_authorization, {}))
                                                    waiting(granted);
                                            });
                                        }];
}

void NotificationBackendMacOS::show(PlatformNotification const& notification)
{
    if (!m_bundled) {
        if (on_failed)
            on_failed(notification.id);
        return;
    }

    request_authorization_then([this, notification](bool granted) {
        // Closed while the authorization was pending: Its close was reported then.
        if (m_closed_before_shown.remove(notification.id))
            return;
        if (!granted) {
            if (on_failed)
                on_failed(notification.id);
            return;
        }

        auto* content = [[UNMutableNotificationContent alloc] init];
        content.title = ns_string(platform_text(notification.data.title));
        content.subtitle = ns_string(notification.attribution);
        content.body = ns_string(platform_text(notification.data.body));
        content.threadIdentifier = ns_string(notification.attribution);
        content.categoryIdentifier = @"org.ladybird.Ladybird.web-notification";
        if (notification.data.silent != true)
            content.sound = [UNNotificationSound defaultSound];

        // A replacement without renotify shouldn't alert again.
        bool replaces = notification.replaces.has_value();
        if (replaces && !notification.data.renotify)
            content.interruptionLevel = UNNotificationInterruptionLevelPassive;

        // The center takes an attachment from a file, which it copies.
        NSURL* icon_file = nil;
        if (notification.icon) {
            if (auto png = Gfx::PNGWriter::encode(*notification.icon); !png.is_error()) {
                auto path = ByteString::formatted("{}ladybird-notification-icon-{}.png", [NSTemporaryDirectory() UTF8String], notification.id);
                icon_file = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.characters()]];
                NSData* data = [NSData dataWithBytes:png.value().data() length:png.value().size()];
                if ([data writeToURL:icon_file atomically:YES]) {
                    NSError* error = nil;
                    if (auto* attachment = [UNNotificationAttachment attachmentWithIdentifier:@"icon" URL:icon_file options:nil error:&error])
                        content.attachments = @[ attachment ];
                } else {
                    icon_file = nil;
                }
            }
        }

        // Reusing the replaced notification's identifier makes the center swap it in place.
        String identifier;
        if (replaces) {
            if (auto existing = m_identifier_for_id.take(*notification.replaces); existing.has_value())
                identifier = existing.release_value();
        }
        if (identifier.is_empty())
            identifier = MUST(String::formatted("{}-{}", m_launch_token, notification.id));
        m_identifier_for_id.set(notification.id, identifier);
        m_id_for_identifier.set(identifier, notification.id);

        auto* request = [UNNotificationRequest requestWithIdentifier:ns_string(identifier) content:content trigger:nil];
        [center_of(*m_bridge) addNotificationRequest:request
                               withCompletionHandler:^(NSError* error) {
                                   if (icon_file)
                                       [[NSFileManager defaultManager] removeItemAtURL:icon_file error:nil];
                                   bool failed = error != nil;
                                   if (failed)
                                       dbgln("Notifications: The notification center refused the request: {}", [[error localizedDescription] UTF8String]);
                                   on_main_thread([this, id = notification.id, failed] {
                                       if (failed) {
                                           if (auto identifier = m_identifier_for_id.take(id); identifier.has_value())
                                               m_id_for_identifier.remove(*identifier);
                                           if (on_failed)
                                               on_failed(id);
                                           return;
                                       }
                                       if (on_shown)
                                           on_shown(id);
                                   });
                               }];
    });
}

void NotificationBackendMacOS::close(u64 id)
{
    auto identifier = m_identifier_for_id.take(id);
    if (!identifier.has_value()) {
        // Not handed to the center yet (authorization pending): The show, when it gets its turn, finds this and
        // does nothing; the close is reported now.
        if (m_authorization == Authorization::Pending) {
            m_closed_before_shown.set(id);
            if (on_closed)
                on_closed(id, ByUser::No);
        }
        return;
    }
    m_id_for_identifier.remove(*identifier);
    NSArray* identifiers = @[ ns_string(*identifier) ];
    [center_of(*m_bridge) removeDeliveredNotificationsWithIdentifiers:identifiers];
    [center_of(*m_bridge) removePendingNotificationRequestsWithIdentifiers:identifiers];
    if (on_closed)
        on_closed(id, ByUser::No);
}

void NotificationBackendMacOS::did_activate(String const& identifier)
{
    auto id = m_id_for_identifier.take(identifier);
    if (!id.has_value())
        return;
    m_identifier_for_id.remove(*id);
    if (on_clicked)
        on_clicked(*id);
    if (on_closed)
        on_closed(*id, ByUser::Yes);
}

void NotificationBackendMacOS::did_dismiss(String const& identifier)
{
    auto id = m_id_for_identifier.take(identifier);
    if (!id.has_value())
        return;
    m_identifier_for_id.remove(*id);
    if (on_closed)
        on_closed(*id, ByUser::Yes);
}

}

@implementation LadybirdNotificationCenterDelegate
{
    WebView::NotificationBackendMacOS* m_backend;
}

- (instancetype)initWithBackend:(WebView::NotificationBackendMacOS*)backend
{
    if ((self = [super init]))
        m_backend = backend;
    return self;
}

- (void)detach
{
    m_backend = nullptr;
}

// Notifications show as banners even while the browser is the frontmost application.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    auto identifier = MUST(String::from_utf8(StringView { [response.notification.request.identifier UTF8String], strlen([response.notification.request.identifier UTF8String]) }));
    bool dismissed = [response.actionIdentifier isEqualToString:UNNotificationDismissActionIdentifier];
    WebView::on_main_thread([self, identifier, dismissed] {
        if (!m_backend)
            return;
        if (dismissed)
            m_backend->did_dismiss(identifier);
        else
            m_backend->did_activate(identifier);
    });
    completionHandler();
}

@end
