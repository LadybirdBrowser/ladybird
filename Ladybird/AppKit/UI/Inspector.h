/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@class LadybirdWebView;
@class Tab;

@interface Inspector : NSWindow

- (instancetype)init:(Tab*)tab;

- (void)inspect;
- (void)reset;

- (void)selectHoveredElement;

@property (nonatomic, strong) LadybirdWebView* web_view;

@end
