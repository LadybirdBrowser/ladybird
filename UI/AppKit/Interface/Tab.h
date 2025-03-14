/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

#import <Cocoa/Cocoa.h>
#import <Interface/LadybirdWebViewWindow.h>

@class LadybirdWebView;

@interface Tab : LadybirdWebViewWindow

- (instancetype)init;
- (instancetype)initAsChild:(Tab*)parent
                  pageIndex:(u64)page_index;

@end
