/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@class Tab;

using InfoBarDismissed = void (^)(void);

@interface InfoBar : NSStackView

- (void)showWithMessage:(NSString*)message
    dismissButtonTooltip:(NSString*)tooltip
    dismissButtonClicked:(InfoBarDismissed)on_dimissed
               activeTab:(Tab*)tab;
- (void)hide;

- (void)tabBecameActive:(Tab*)tab;

@end
