/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>
#import <Ladybird/AppKit/UI/LadybirdWebViewWindow.h>

@class LadybirdWebView;
@class Tab;

@interface Inspector : LadybirdWebViewWindow

- (instancetype)init:(Tab*)tab;

- (void)inspect;
- (void)reset;

- (void)selectHoveredElement;

@end
