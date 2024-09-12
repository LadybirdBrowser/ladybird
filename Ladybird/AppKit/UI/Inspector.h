/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@class LadybirdWebView;
@class Tab;

@interface Inspector : NSScrollView

- (instancetype)init:(Tab*)tab
          isWindowed:(BOOL)is_windowed;

- (void)inspect;
- (void)reset;

- (void)selectHoveredElement;

- (void)setIsWindowed:(BOOL)is_windowed;

@property (nonatomic, strong) NSScrollView* inspector_scroll_view;
@property (nonatomic, strong) LadybirdWebView* web_view;

@end
