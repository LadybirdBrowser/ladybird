/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import Foundation
import Ladybird.WebView
import Ladybird.WebViewApplication
import Ladybird.WebViewWindow
import SwiftUI

public class TaskManager: LadybirdWebViewWindow {

    private let WINDOW_WIDTH: CGFloat = 600
    private let WINDOW_HEIGHT: CGFloat = 400

    private var timer: Timer?

    init() {
        let tab_rect = NSApplication.shared.keyWindow!.frame
        let position_x = tab_rect.origin.x + (tab_rect.size.width - WINDOW_WIDTH) / 2
        let position_y = tab_rect.origin.y + (tab_rect.size.height - WINDOW_HEIGHT) / 2
        let window_rect = NSMakeRect(position_x, position_y, WINDOW_WIDTH, WINDOW_HEIGHT)

        super.init(webView: nil, windowRect: window_rect)

        self.timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] timer in
            if let strong_self = self {
                strong_self.updateStatistics()
            }
        }

        self.contentView = self.web_view
        self.title = "Task Manager"
        self.setIsVisible(true)

        self.updateStatistics()
    }

    func updateStatistics() {
        WebView.Application.the().update_process_statistics()
        self.web_view.loadHTML(WebView.Application.the().generate_process_statistics_html().__bytes_as_string_viewUnsafe())
    }
}
