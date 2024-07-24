/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import Foundation
import SwiftUI

@objc
public protocol TaskManagerDelegate where Self: NSObject {
    func onTaskManagerClosed()
}

public class TaskManagerController: NSWindowController, NSWindowDelegate {

    private weak var delegate: TaskManagerDelegate?

    @objc
    public convenience init(delegate: TaskManagerDelegate) {
        self.init()
        self.delegate = delegate
    }

    @IBAction public override func showWindow(_ sender: Any?) {
        self.window = TaskManager.init()
        self.window!.delegate = self
        self.window!.makeKeyAndOrderFront(sender)
    }

    public func windowWillClose(_ sender: Notification) {
        self.delegate?.onTaskManagerClosed()
    }

    public func windowDidResize(_ sender: Notification) {
        guard self.window != nil else { return }
        if !self.window!.inLiveResize {
            self.taskManager().web_view.handleResize()
        }
    }

    public func windowDidChangeBackingProperties(_ sender: Notification) {
        self.taskManager().web_view.handleDevicePixelRatioChange()
    }

    private func taskManager() -> TaskManager {
        return self.window as! TaskManager
    }
}
