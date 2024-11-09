/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibIPC/Forward.h>
#include <LibMain/Main.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {
class WebViewBridge;
}

@interface Application : NSApplication

- (void)setupWebViewApplication:(Main::Arguments&)arguments
                  newTabPageURL:(URL::URL)new_tab_page_url;

- (ErrorOr<void>)launchRequestServer;
- (ErrorOr<void>)launchImageDecoder;
- (ErrorOr<NonnullRefPtr<WebView::WebContentClient>>)launchWebContent:(Ladybird::WebViewBridge&)web_view_bridge;
- (ErrorOr<IPC::File>)launchWebWorker;

@end
