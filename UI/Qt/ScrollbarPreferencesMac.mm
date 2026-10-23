/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Cocoa/Cocoa.h>
#import <UI/Qt/ScrollbarPreferencesMac.h>

namespace Ladybird {

bool scrollbar_track_press_jumps()
{
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"AppleScrollerPagingBehavior"];
}

void on_scrollbar_track_press_behavior_change(Function<void()> callback)
{
    // NB: The system announces a change of this setting with the notification that Chromium also watches for it.
    static Function<void()> s_callback;
    auto should_observe = !s_callback;
    s_callback = move(callback);
    if (!should_observe)
        return;
    [NSDistributedNotificationCenter.defaultCenter addObserverForName:@"AppleNoRedisplayAppearancePreferenceChanged"
                                                               object:nil
                                                                queue:NSOperationQueue.mainQueue
                                                           usingBlock:^(NSNotification*) {
                                                               if (s_callback)
                                                                   s_callback();
                                                           }];
}

}
