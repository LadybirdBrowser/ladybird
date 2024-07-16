/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import Foundation
import Ladybird.WebView
import Ladybird.WebViewApplication
import SwiftUI

public class TaskManager: NSWindow {

  private let WINDOW_WIDTH: CGFloat = 600
  private let WINDOW_HEIGHT: CGFloat = 400

  var web_view: LadybirdWebView
  private var timer: Timer?

  init() {
    let tab_rect = NSApplication.shared.keyWindow!.frame
    let position_x = tab_rect.origin.x + (tab_rect.size.width - WINDOW_WIDTH) / 2
    let position_y = tab_rect.origin.y + (tab_rect.size.height - WINDOW_HEIGHT) / 2

    let window_rect = NSMakeRect(position_x, position_y, WINDOW_WIDTH, WINDOW_HEIGHT)
    let style_mask = NSWindow.StyleMask.init(arrayLiteral: [
      NSWindow.StyleMask.titled, NSWindow.StyleMask.closable, NSWindow.StyleMask.miniaturizable,
      NSWindow.StyleMask.resizable,
    ])

    self.web_view = LadybirdWebView.init(nil)

    super.init(
      contentRect: window_rect, styleMask: style_mask, backing: NSWindow.BackingStoreType.buffered,
      defer: false)

    self.timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] timer in
      if let strong_self = self {
        strong_self.updateStatistics()
      }
    }

    self.web_view.postsBoundsChangedNotifications = true
    let scroll_view = NSScrollView()
    scroll_view.hasVerticalScroller = true
    scroll_view.hasHorizontalScroller = true
    scroll_view.lineScroll = 24

    scroll_view.contentView = self.web_view
    scroll_view.documentView = NSView()

    self.contentView = scroll_view
    self.title = "Task Manager"
    self.setIsVisible(true)

    self.updateStatistics()
  }

  func updateStatistics() {
    WebView.Application.the().update_process_statistics();
    self.web_view.loadHTML(WebView.Application.the().generate_process_statistics_html().__bytes_as_string_viewUnsafe());
  }
}
