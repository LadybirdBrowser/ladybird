/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/SystemAppearance.h>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <QMetaObject>

namespace Ladybird {

// NB: The application's own appearance follows the interface theme, so the system's choice is read from the global
//     defaults instead. The key only exists while the system is dark, including while Auto has made it dark.
bool SystemAppearance::query_system_prefers_dark() const
{
    auto* style = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    return [style isEqualToString:@"Dark"];
}

bool SystemAppearance::prefers_reduced_motion()
{
    return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
}

void SystemAppearance::observe_platform_changes()
{
    [[NSDistributedNotificationCenter defaultCenter] addObserverForName:@"AppleInterfaceThemeChangedNotification"
                                                                 object:nil
                                                                  queue:[NSOperationQueue mainQueue]
                                                             usingBlock:^(NSNotification*) {
                                                                 // The defaults are updated after the notification is posted.
                                                                 QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
                                                             }];
}

}
