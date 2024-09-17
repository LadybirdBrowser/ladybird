/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

#import <Cocoa/Cocoa.h>

@class LadybirdWebView;

@interface Tab : NSWindow

- (instancetype)init;
- (instancetype)initAsChild:(Tab*)parent
                  pageIndex:(u64)page_index;

- (void)tabWillClose;

- (void)openInspector:(id)sender;
- (void)onInspectorClosed;

@property (nonatomic, strong) LadybirdWebView* web_view;

@end
