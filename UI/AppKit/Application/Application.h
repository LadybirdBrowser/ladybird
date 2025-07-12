/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Application.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

private:
    explicit Application();

    virtual Optional<ByteString> ask_user_for_download_folder() const override;
    virtual NonnullOwnPtr<Core::EventLoop> create_platform_event_loop() override;
};

}

@interface Application : NSApplication
@end
