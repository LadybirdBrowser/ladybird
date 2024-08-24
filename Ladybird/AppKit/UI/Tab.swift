/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Douwe Zumker <douwezumker@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import Foundation
import AppKit
import Ladybird.Utilities
import LibCore
import LibGfx
import LibURL
import LibWebView
import SwiftUI
public class Tab: NSWindow,LadybirdWebViewObserver {

    private let WINDOW_WIDTH: CGFloat = 1000
    private let WINDOW_HEIGHT: CGFloat = 800

    var title: String {
        get {
            self.attributedTitle?.string ?? ""
        }
        set {
            self.updateTabTitleAndFavicon()
        }
    }

    var favicon: NSImage?
    var searchPanel: SearchPanel
    var inspectorController: InspectorController?

    static var defaultFavicon: NSImage {
        struct Static {
            static let defaultFavicon: NSImage = {
                let defaultFaviconPath = try! Core.Resource.load_from_uri("resource://icons/48x48/app-browser.png" as StaticString)
                let nsDefaultFaviconPath = Ladybird.string_to_ns_string(defaultFaviconPath.filesystem_path()!)
                return NSImage(contentsOfFile: nsDefaultFaviconPath)!
            }()
        }
        return Static.defaultFavicon
    }

    init() {
        let screenRect = NSScreen.main!.frame
        let positionX = (screenRect.width - WINDOW_WIDTH) / 2
        let positionY = (screenRect.height - WINDOW_HEIGHT) / 2

        let windowRect = NSMakeRect(positionX, positionY, WINDOW_WIDTH, WINDOW_HEIGHT)
        let styleMask: NSWindow.StyleMask = [.titled, .closable, .miniaturizable, .resizable]

        self.searchPanel = SearchPanel()
        super.init(contentRect: windowRect, styleMask: styleMask, backing: .buffered, defer: false)

        self.frameAutosaveName = "window"
        self.favicon = Tab.defaultFavicon
        self.title = "New Tab"

        let webView = LadybirdWebView(observer: self)
        webView.postsBoundsChangedNotifications = true
        self.contentView = webView

        self.updateTabTitleAndFavicon()
        self.titleVisibility = .hidden
        self.isVisible = true

        self.searchPanel.isHidden = true

        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = false
        scrollView.hasHorizontalScroller = false
        scrollView.lineScroll = 24
        scrollView.contentView = webView
        scrollView.documentView = NSView()

        let stackView = NSStackView(views: [self.searchPanel, scrollView])
        stackView.orientation = .vertical
        stackView.spacing = 0
        self.contentView = stackView

        NotificationCenter.default.addObserver(self, selector: #selector(onContentScroll(_)), name: NSView.boundsDidChangeNotification, object: scrollView.contentView)
        NSLayoutConstraint.activate([self.searchPanel.leadingAnchor.constraint(equalTo: self.contentView!.leadingAnchor)])
    }

    func find(_ sender: Any?) {
        self.searchPanel.find(sender)
    }

    func findNextMatch(_ sender: Any?) {
        self.searchPanel.findNextMatch(sender)
    }

    func findPreviousMatch(_ sender: Any?) {
        self.searchPanel.findPreviousMatch(sender)
    }

    func useSelectionForFind(_ sender: Any?) {
        self.searchPanel.useSelectionForFind(sender)
    }

    func tabWillClose() {
        if let inspectorController = self.inspectorController {
            inspectorController.window.close()
        }
    }

    func openInspector(_ sender: Any?) {
        if let inspectorController = self.inspectorController {
            inspectorController.window.makeKeyAndOrderFront(sender)
            return
        }

        let inspectorController = InspectorController(tab: self)
        inspectorController.showWindow(nil)
        self.inspectorController = inspectorController
    }

    func onInspectorClosed() {
        self.inspectorController = nil
    }

    func inspectElement(_ sender: Any?) {
        self.openInspector(sender)

        guard let inspector = self.inspectorController?.window as? Inspector else { return }
        inspector.selectHoveredElement()
    }

    // Private Methods

    private func tabController() -> TabController? {
        return self.windowController as? TabController
    }

    private func updateTabTitleAndFavicon() {
        let faviconAttachment = NSTextAttachment()
        faviconAttachment.image = self.favicon

        let faviconAttribute = NSMutableAttributedString(attributedString: NSAttributedString(attachment: faviconAttachment))
        faviconAttribute.addAttribute(.foregroundColor, value: NSColor.clear, range: NSRange(location: 0, length: faviconAttribute.length))

        let titleAttributes: [NSAttributedString.Key: Any] = [
            .foregroundColor: NSColor.textColor,
            .baselineOffset: 3
        ]

        let titleAttribute = NSAttributedString(string: self.title, attributes: titleAttributes)
        let spacingAttribute = NSAttributedString(string: "  ")

        let titleAndFavicon = NSMutableAttributedString()
        titleAndFavicon.append(faviconAttribute)
        titleAndFavicon.append(spacingAttribute)
        titleAndFavicon.append(titleAttribute)

        self.tab?.attributedTitle = titleAndFavicon
    }

    private func togglePageMuteState(_ button: NSButton) {
        guard let view = self.contentView as? LadybirdWebView else { return }
        view.view.toggle_page_mute_state()

        switch view.view.audio_play_state() {
        case .paused:
            self.tab?.accessoryView = nil
        case .playing:
            button.image = self.iconForPageMuteState()
            button.toolTip = self.toolTipForPageMuteState()
        }
    }

    private func iconForPageMuteState() -> NSImage {
        guard let view = self.contentView as? LadybirdWebView else { fatalError("View not found") }

        switch view.view.page_mute_state() {
        case .muted:
            return NSImage(named: NSImage.touchBarAudioOutputVolumeOffTemplateName)!
        case .unmuted:
            return NSImage(named: NSImage.touchBarAudioOutputVolumeHighTemplateName)!
        @unknown default:
            fatalError("Unknown MuteState")
        }
    }

    private func toolTipForPageMuteState() -> String {
        guard let view = self.contentView as? LadybirdWebView else { fatalError("View not found") }

        switch view.view.page_mute_state() {
        case .muted:
            return "Unmute tab"
        case .unmuted:
            return "Mute tab"
        @unknown default:
            fatalError("Unknown MuteState")
        }
    }

    @objc private func onContentScroll(_ notification: Notification) {
        guard let webView = self.contentView as? LadybirdWebView else { return }
        webView.handleScroll()
    }

    // LadybirdWebViewObserver Protocol

    func onCreateNewTab(_ url: URL, activateTab: Web.HTML.ActivateTab) -> String {
        let delegate = NSApp.delegate as! ApplicationDelegate
        let controller = delegate.createNewTab(url, fromTab: self, activateTab: activateTab)
        let tab = controller.window as! Tab
        return tab.web_view.handle()
    }

    func onCreateNewTab(_ html: StringView, url: URL, activateTab: Web.HTML.ActivateTab) -> String {
        let delegate = NSApp.delegate as! ApplicationDelegate
        let controller = delegate.createNewTab(html, url: url, fromTab: self, activateTab: activateTab)
        let tab = controller.window as! Tab
        return tab.web_view.handle()
    }

    func loadURL(_ url: URL) {
        self.tabController()?.loadURL(url)
    }

    func onLoadStart(_ url: URL, isRedirect: Bool) {
        self.title = Ladybird.string_to_ns_string(url.serialize())
        self.favicon = Tab.defaultFavicon
        self.updateTabTitleAndFavicon()

        self.tabController()?.onLoadStart(url, isRedirect: isRedirect)

        if let inspectorController = self.inspectorController {
            let inspector = inspectorController.window as! Inspector
            inspector.reset()
        }
    }

    func onLoadFinish(_ url: URL) {
        if let inspectorController = self.inspectorController {
            let inspector = inspectorController.window as! Inspector
            inspector.inspect()
        }
    }

    func onURLChange(_ url: URL) {
        self.tabController()?.onURLChange(url)
    }

    func onBackNavigationEnabled(_ backEnabled: Bool, forwardNavigationEnabled: Bool) {
        self.tabController()?.onBackNavigationEnabled(backEnabled, forwardNavigationEnabled: forwardNavigationEnabled)
    }

    func onTitleChange(_ title: ByteString) {
        self.tabController()?.onTitleChange(title)
        self.title = Ladybird.string_to_ns_string(title)
        self.updateTabTitleAndFavicon()
    }

    func onFaviconChange(_ bitmap: Gfx.Bitmap) {
        let faviconSize: CGFloat = 16

        guard let png = try? Gfx.PNGWriter.encode(bitmap).get() else {
            return
        }

        let data = NSData(bytes: png.data(), length: png.size())
        let favicon = NSImage(data: data as Data)!
        favicon.resizingMode = .stretch
        favicon.size = NSSize(width: faviconSize, height: faviconSize)

        self.favicon = favicon
        self.updateTabTitleAndFavicon()
    }

    func onAudioPlayStateChange(_ playState: Web.HTML.AudioPlayState) {
        guard let view = (self.web_view as? LadybirdWebView)?.view else { return }

        switch playState {
        case .paused:
            if view.page_mute_state() == .unmuted {
                self.tab?.accessoryView = nil
            }
        case .playing:
            let button = NSButton(image: self.iconForPageMuteState(), target: self, action: #selector(togglePageMuteState(_)))
            button.toolTip = self.toolTipForPageMuteState()
            self.tab?.accessoryView = button
        @unknown default:
            fatalError("Unknown AudioPlayState")
        }
    }

    func onFindInPageResult(currentMatchIndex: Int, totalMatchCount: Int?) {
        self.searchPanel.onFindInPageResult(currentMatchIndex: currentMatchIndex, totalMatchCount: totalMatchCount)
    }

    override var isVisible: Bool {
        didSet {
            (self.web_view as? LadybirdWebView)?.handleVisibility(isVisible)
            super.isVisible = isVisible
        }
    }

    override var isMiniaturized: Bool {
        didSet {
        (self.web_view as? LadybirdWebView)?.handleVisibility(!isMiniaturized)
        super.isMiniaturized = isMiniaturized
        }
    }
}
