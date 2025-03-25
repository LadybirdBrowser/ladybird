/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>

#import <Application/ApplicationDelegate.h>
#import <Interface/InfoBar.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>
#import <LibWebView/UserAgent.h>

#import <Utilities/Conversions.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

@interface ApplicationDelegate ()
{
    Web::CSS::PreferredColorScheme m_preferred_color_scheme;
    Web::CSS::PreferredContrast m_preferred_contrast;
    Web::CSS::PreferredMotion m_preferred_motion;
    ByteString m_navigator_compatibility_mode;
}

@property (nonatomic, strong) NSMutableArray<TabController*>* managed_tabs;
@property (nonatomic, weak) Tab* active_tab;

@property (nonatomic, strong) InfoBar* info_bar;

@property (nonatomic, strong) NSMenuItem* toggle_devtools_menu_item;

- (NSMenuItem*)createApplicationMenu;
- (NSMenuItem*)createFileMenu;
- (NSMenuItem*)createEditMenu;
- (NSMenuItem*)createViewMenu;
- (NSMenuItem*)createSettingsMenu;
- (NSMenuItem*)createHistoryMenu;
- (NSMenuItem*)createInspectMenu;
- (NSMenuItem*)createDebugMenu;
- (NSMenuItem*)createWindowMenu;
- (NSMenuItem*)createHelpMenu;

@end

@implementation ApplicationDelegate

- (instancetype)init
{
    if (self = [super init]) {
        [NSApp setMainMenu:[[NSMenu alloc] init]];

        [[NSApp mainMenu] addItem:[self createApplicationMenu]];
        [[NSApp mainMenu] addItem:[self createFileMenu]];
        [[NSApp mainMenu] addItem:[self createEditMenu]];
        [[NSApp mainMenu] addItem:[self createViewMenu]];
        [[NSApp mainMenu] addItem:[self createSettingsMenu]];
        [[NSApp mainMenu] addItem:[self createHistoryMenu]];
        [[NSApp mainMenu] addItem:[self createInspectMenu]];
        [[NSApp mainMenu] addItem:[self createDebugMenu]];
        [[NSApp mainMenu] addItem:[self createWindowMenu]];
        [[NSApp mainMenu] addItem:[self createHelpMenu]];

        self.managed_tabs = [[NSMutableArray alloc] init];

        m_preferred_color_scheme = Web::CSS::PreferredColorScheme::Auto;
        m_preferred_contrast = Web::CSS::PreferredContrast::Auto;
        m_preferred_motion = Web::CSS::PreferredMotion::Auto;
        m_navigator_compatibility_mode = "chrome";

        // Reduce the tooltip delay, as the default delay feels quite long.
        [[NSUserDefaults standardUserDefaults] setObject:@100 forKey:@"NSInitialToolTipDelay"];
    }

    return self;
}

#pragma mark - Public methods

- (TabController*)createNewTab:(Optional<URL::URL> const&)url
                       fromTab:(Tab*)tab
                   activateTab:(Web::HTML::ActivateTab)activate_tab
{
    auto* controller = [self createNewTab:activate_tab fromTab:tab];

    if (url.has_value()) {
        [controller loadURL:*url];
    }

    return controller;
}

- (nonnull TabController*)createNewTab:(StringView)html
                                   url:(URL::URL const&)url
                               fromTab:(nullable Tab*)tab
                           activateTab:(Web::HTML::ActivateTab)activate_tab
{
    auto* controller = [self createNewTab:activate_tab fromTab:tab];
    [controller loadHTML:html url:url];

    return controller;
}

- (nonnull TabController*)createChildTab:(Optional<URL::URL> const&)url
                                 fromTab:(nonnull Tab*)tab
                             activateTab:(Web::HTML::ActivateTab)activate_tab
                               pageIndex:(u64)page_index
{
    auto* controller = [self createChildTab:activate_tab fromTab:tab pageIndex:page_index];

    if (url.has_value()) {
        [controller loadURL:*url];
    }

    return controller;
}

- (void)setActiveTab:(Tab*)tab
{
    self.active_tab = tab;

    if (self.info_bar) {
        [self.info_bar tabBecameActive:self.active_tab];
    }
}

- (Tab*)activeTab
{
    return self.active_tab;
}

- (void)removeTab:(TabController*)controller
{
    [self.managed_tabs removeObject:controller];
}

- (Web::CSS::PreferredColorScheme)preferredColorScheme
{
    return m_preferred_color_scheme;
}

- (Web::CSS::PreferredContrast)preferredContrast
{
    return m_preferred_contrast;
}

- (Web::CSS::PreferredMotion)preferredMotion
{
    return m_preferred_motion;
}

#pragma mark - Private methods

- (void)openAboutVersionPage:(id)sender
{
    auto* current_tab = [NSApp keyWindow];
    if (![current_tab isKindOfClass:[Tab class]]) {
        return;
    }

    [self createNewTab:URL::URL(URL::about_version())
               fromTab:(Tab*)current_tab
           activateTab:Web::HTML::ActivateTab::Yes];
}

- (nonnull TabController*)createNewTab:(Web::HTML::ActivateTab)activate_tab
                               fromTab:(nullable Tab*)tab
{
    auto* controller = [[TabController alloc] init];
    [self initializeTabController:controller
                      activateTab:activate_tab
                          fromTab:tab];

    return controller;
}

- (nonnull TabController*)createChildTab:(Web::HTML::ActivateTab)activate_tab
                                 fromTab:(nonnull Tab*)tab
                               pageIndex:(u64)page_index
{
    auto* controller = [[TabController alloc] initAsChild:tab pageIndex:page_index];
    [self initializeTabController:controller
                      activateTab:activate_tab
                          fromTab:tab];

    return controller;
}

- (void)initializeTabController:(TabController*)controller
                    activateTab:(Web::HTML::ActivateTab)activate_tab
                        fromTab:(nullable Tab*)tab
{
    [controller showWindow:nil];

    if (tab) {
        [[tab tabGroup] addWindow:controller.window];

        // FIXME: Can we create the tabbed window above without it becoming active in the first place?
        if (activate_tab == Web::HTML::ActivateTab::No) {
            [tab orderFront:nil];
        }
    }

    if (activate_tab == Web::HTML::ActivateTab::Yes) {
        [[controller window] orderFrontRegardless];
    }

    [self.managed_tabs addObject:controller];
    [controller onCreateNewTab];
}

- (void)openSettings:(id)sender
{
    [self createNewTab:URL::URL::about("settings"_string)
               fromTab:self.active_tab
           activateTab:Web::HTML::ActivateTab::Yes];
}

- (void)closeCurrentTab:(id)sender
{
    auto* current_window = [NSApp keyWindow];
    [current_window close];
}

- (void)toggleDevToolsEnabled:(id)sender
{
    if (auto result = WebView::Application::the().toggle_devtools_enabled(); result.is_error()) {
        auto error_message = MUST(String::formatted("Unable to start DevTools: {}", result.error()));

        auto* dialog = [[NSAlert alloc] init];
        [dialog setMessageText:Ladybird::string_to_ns_string(error_message)];

        [dialog beginSheetModalForWindow:self.active_tab
                       completionHandler:nil];
    } else {
        switch (result.value()) {
        case WebView::Application::DevtoolsState::Disabled:
            [self devtoolsDisabled];
            break;
        case WebView::Application::DevtoolsState::Enabled:
            [self devtoolsEnabled];
            break;
        }
    }
}

- (void)devtoolsDisabled
{
    [self.toggle_devtools_menu_item setTitle:@"Enable DevTools"];

    if (self.info_bar) {
        [self.info_bar hide];
        self.info_bar = nil;
    }
}

- (void)devtoolsEnabled
{
    [self.toggle_devtools_menu_item setTitle:@"Disable DevTools"];

    if (!self.info_bar) {
        self.info_bar = [[InfoBar alloc] init];
    }

    auto message = MUST(String::formatted("DevTools is enabled on port {}", WebView::Application::browser_options().devtools_port));

    [self.info_bar showWithMessage:Ladybird::string_to_ns_string(message)
                dismissButtonTitle:@"Disable"
              dismissButtonClicked:^{
                  MUST(WebView::Application::the().toggle_devtools_enabled());
                  [self devtoolsDisabled];
              }
                         activeTab:self.active_tab];
}

- (void)openTaskManager:(id)sender
{
    [self createNewTab:URL::URL::about("processes"_string)
               fromTab:self.active_tab
           activateTab:Web::HTML::ActivateTab::Yes];
}

- (void)openLocation:(id)sender
{
    auto* current_tab = [NSApp keyWindow];

    if (![current_tab isKindOfClass:[Tab class]]) {
        return;
    }

    auto* controller = (TabController*)[current_tab windowController];
    [controller focusLocationToolbarItem];
}

- (void)setAutoPreferredColorScheme:(id)sender
{
    m_preferred_color_scheme = Web::CSS::PreferredColorScheme::Auto;
    [self broadcastPreferredColorSchemeUpdate];
}

- (void)setDarkPreferredColorScheme:(id)sender
{
    m_preferred_color_scheme = Web::CSS::PreferredColorScheme::Dark;
    [self broadcastPreferredColorSchemeUpdate];
}

- (void)setLightPreferredColorScheme:(id)sender
{
    m_preferred_color_scheme = Web::CSS::PreferredColorScheme::Light;
    [self broadcastPreferredColorSchemeUpdate];
}

- (void)broadcastPreferredColorSchemeUpdate
{
    for (TabController* controller in self.managed_tabs) {
        auto* tab = (Tab*)[controller window];
        [[tab web_view] setPreferredColorScheme:m_preferred_color_scheme];
    }
}

- (void)setAutoPreferredContrast:(id)sender
{
    m_preferred_contrast = Web::CSS::PreferredContrast::Auto;
    [self broadcastPreferredContrastUpdate];
}

- (void)setLessPreferredContrast:(id)sender
{
    m_preferred_contrast = Web::CSS::PreferredContrast::Less;
    [self broadcastPreferredContrastUpdate];
}

- (void)setMorePreferredContrast:(id)sender
{
    m_preferred_contrast = Web::CSS::PreferredContrast::More;
    [self broadcastPreferredContrastUpdate];
}

- (void)setNoPreferencePreferredContrast:(id)sender
{
    m_preferred_contrast = Web::CSS::PreferredContrast::NoPreference;
    [self broadcastPreferredContrastUpdate];
}

- (void)broadcastPreferredContrastUpdate
{
    for (TabController* controller in self.managed_tabs) {
        auto* tab = (Tab*)[controller window];
        [[tab web_view] setPreferredContrast:m_preferred_contrast];
    }
}

- (void)setAutoPreferredMotion:(id)sender
{
    m_preferred_motion = Web::CSS::PreferredMotion::Auto;
    [self broadcastPreferredMotionUpdate];
}

- (void)setNoPreferencePreferredMotion:(id)sender
{
    m_preferred_motion = Web::CSS::PreferredMotion::NoPreference;
    [self broadcastPreferredMotionUpdate];
}

- (void)setReducePreferredMotion:(id)sender
{
    m_preferred_motion = Web::CSS::PreferredMotion::Reduce;
    [self broadcastPreferredMotionUpdate];
}

- (void)broadcastPreferredMotionUpdate
{
    for (TabController* controller in self.managed_tabs) {
        auto* tab = (Tab*)[controller window];
        [[tab web_view] setPreferredMotion:m_preferred_motion];
    }
}

- (void)clearHistory:(id)sender
{
    for (TabController* controller in self.managed_tabs) {
        [controller clearHistory];
    }
}

- (void)dumpCookies:(id)sender
{
    WebView::Application::cookie_jar().dump_cookies();
}

- (void)clearAllCookies:(id)sender
{
    WebView::Application::cookie_jar().clear_all_cookies();
}

- (NSMenuItem*)createApplicationMenu
{
    auto* menu = [[NSMenuItem alloc] init];

    auto* process_name = [[NSProcessInfo processInfo] processName];
    auto* submenu = [[NSMenu alloc] initWithTitle:process_name];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"About %@", process_name]
                                                action:@selector(openAboutVersionPage:)
                                         keyEquivalent:@""]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Settings"
                                                action:@selector(openSettings:)
                                         keyEquivalent:@","]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Hide %@", process_name]
                                                action:@selector(hide:)
                                         keyEquivalent:@"h"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Quit %@", process_name]
                                                action:@selector(terminate:)
                                         keyEquivalent:@"q"]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createFileMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"File"];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"New Tab"
                                                action:@selector(createNewTab:)
                                         keyEquivalent:@"t"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Close Tab"
                                                action:@selector(closeCurrentTab:)
                                         keyEquivalent:@"w"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Location"
                                                action:@selector(openLocation:)
                                         keyEquivalent:@"l"]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createEditMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"Edit"];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Undo"
                                                action:@selector(undo:)
                                         keyEquivalent:@"z"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Redo"
                                                action:@selector(redo:)
                                         keyEquivalent:@"y"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Cut"
                                                action:@selector(cut:)
                                         keyEquivalent:@"x"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy"
                                                action:@selector(copy:)
                                         keyEquivalent:@"c"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Paste"
                                                action:@selector(paste:)
                                         keyEquivalent:@"v"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Select All"
                                                action:@selector(selectAll:)
                                         keyEquivalent:@"a"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Find..."
                                                action:@selector(find:)
                                         keyEquivalent:@"f"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Find Next"
                                                action:@selector(findNextMatch:)
                                         keyEquivalent:@"g"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Find Previous"
                                                action:@selector(findPreviousMatch:)
                                         keyEquivalent:@"G"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Use Selection for Find"
                                                action:@selector(useSelectionForFind:)
                                         keyEquivalent:@"e"]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createViewMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"View"];

    auto* color_scheme_menu = [[NSMenu alloc] init];
    [color_scheme_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Auto"
                                                          action:@selector(setAutoPreferredColorScheme:)
                                                   keyEquivalent:@""]];
    [color_scheme_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Dark"
                                                          action:@selector(setDarkPreferredColorScheme:)
                                                   keyEquivalent:@""]];
    [color_scheme_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Light"
                                                          action:@selector(setLightPreferredColorScheme:)
                                                   keyEquivalent:@""]];

    auto* color_scheme_menu_item = [[NSMenuItem alloc] initWithTitle:@"Color Scheme"
                                                              action:nil
                                                       keyEquivalent:@""];
    [color_scheme_menu_item setSubmenu:color_scheme_menu];

    auto* contrast_menu = [[NSMenu alloc] init];
    [contrast_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Auto"
                                                      action:@selector(setAutoPreferredContrast:)
                                               keyEquivalent:@""]];
    [contrast_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Less"
                                                      action:@selector(setLessPreferredContrast:)
                                               keyEquivalent:@""]];
    [contrast_menu addItem:[[NSMenuItem alloc] initWithTitle:@"More"
                                                      action:@selector(setMorePreferredContrast:)
                                               keyEquivalent:@""]];
    [contrast_menu addItem:[[NSMenuItem alloc] initWithTitle:@"No Preference"
                                                      action:@selector(setNoPreferencePreferredContrast:)
                                               keyEquivalent:@""]];

    auto* contrast_menu_item = [[NSMenuItem alloc] initWithTitle:@"Contrast"
                                                          action:nil
                                                   keyEquivalent:@""];
    [contrast_menu_item setSubmenu:contrast_menu];

    auto* motion_menu = [[NSMenu alloc] init];
    [motion_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Auto"
                                                    action:@selector(setAutoPreferredMotion:)
                                             keyEquivalent:@""]];
    [motion_menu addItem:[[NSMenuItem alloc] initWithTitle:@"No Preference"
                                                    action:@selector(setNoPreferencePreferredMotion:)
                                             keyEquivalent:@""]];
    [motion_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Reduce"
                                                    action:@selector(setReducePreferredMotion:)
                                             keyEquivalent:@""]];

    auto* motion_menu_item = [[NSMenuItem alloc] initWithTitle:@"Motion"
                                                        action:nil
                                                 keyEquivalent:@""];
    [motion_menu_item setSubmenu:motion_menu];

    auto* zoom_menu = [[NSMenu alloc] init];
    [zoom_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Zoom In"
                                                  action:@selector(zoomIn:)
                                           keyEquivalent:@"+"]];
    [zoom_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Zoom Out"
                                                  action:@selector(zoomOut:)
                                           keyEquivalent:@"-"]];
    [zoom_menu addItem:[[NSMenuItem alloc] initWithTitle:@"Actual Size"
                                                  action:@selector(resetZoom:)
                                           keyEquivalent:@"0"]];

    auto* zoom_menu_item = [[NSMenuItem alloc] initWithTitle:@"Zoom"
                                                      action:nil
                                               keyEquivalent:@""];
    [zoom_menu_item setSubmenu:zoom_menu];

    [submenu addItem:color_scheme_menu_item];
    [submenu addItem:contrast_menu_item];
    [submenu addItem:motion_menu_item];
    [submenu addItem:zoom_menu_item];
    [submenu addItem:[NSMenuItem separatorItem]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createSettingsMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"Settings"];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Enable Autoplay"
                                                action:@selector(toggleAutoplay:)
                                         keyEquivalent:@""]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createHistoryMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"History"];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Reload Page"
                                                action:@selector(reload:)
                                         keyEquivalent:@"r"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Navigate Back"
                                                action:@selector(navigateBack:)
                                         keyEquivalent:@"["]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Navigate Forward"
                                                action:@selector(navigateForward:)
                                         keyEquivalent:@"]"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Clear History"
                                                action:@selector(clearHistory:)
                                         keyEquivalent:@""]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createInspectMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"Inspect"];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"View Source"
                                                action:@selector(viewSource:)
                                         keyEquivalent:@"u"]];

    self.toggle_devtools_menu_item = [[NSMenuItem alloc] initWithTitle:@"Enable DevTools"
                                                                action:@selector(toggleDevToolsEnabled:)
                                                         keyEquivalent:@"I"];
    [submenu addItem:self.toggle_devtools_menu_item];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Open Task Manager"
                                                action:@selector(openTaskManager:)
                                         keyEquivalent:@"M"]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createDebugMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"Debug"];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump DOM Tree"
                                                action:@selector(dumpDOMTree:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump Layout Tree"
                                                action:@selector(dumpLayoutTree:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump Paint Tree"
                                                action:@selector(dumpPaintTree:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump Stacking Context Tree"
                                                action:@selector(dumpStackingContextTree:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump Style Sheets"
                                                action:@selector(dumpStyleSheets:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump All Resolved Styles"
                                                action:@selector(dumpAllResolvedStyles:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump History"
                                                action:@selector(dumpHistory:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump Cookies"
                                                action:@selector(dumpCookies:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump Local Storage"
                                                action:@selector(dumpLocalStorage:)
                                         keyEquivalent:@""]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Show Line Box Borders"
                                                action:@selector(toggleLineBoxBorders:)
                                         keyEquivalent:@""]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Collect Garbage"
                                                action:@selector(collectGarbage:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Dump GC Graph"
                                                action:@selector(dumpGCGraph:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Clear Cache"
                                                action:@selector(clearCache:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Clear All Cookies"
                                                action:@selector(clearAllCookies:)
                                         keyEquivalent:@""]];

    [submenu addItem:[NSMenuItem separatorItem]];

    auto* spoof_user_agent_menu = [[NSMenu alloc] init];
    auto add_user_agent = [spoof_user_agent_menu](ByteString name) {
        [spoof_user_agent_menu addItem:[[NSMenuItem alloc] initWithTitle:Ladybird::string_to_ns_string(name)
                                                                  action:@selector(setUserAgentSpoof:)
                                                           keyEquivalent:@""]];
    };

    add_user_agent("Disabled");
    for (auto const& userAgent : WebView::user_agents)
        add_user_agent(userAgent.key);

    auto* spoof_user_agent_menu_item = [[NSMenuItem alloc] initWithTitle:@"Spoof User Agent"
                                                                  action:nil
                                                           keyEquivalent:@""];
    [spoof_user_agent_menu_item setSubmenu:spoof_user_agent_menu];

    [submenu addItem:spoof_user_agent_menu_item];

    auto* navigator_compatibility_mode_menu = [[NSMenu alloc] init];
    auto add_navigator_compatibility_mode = [navigator_compatibility_mode_menu](ByteString name) {
        [navigator_compatibility_mode_menu addItem:[[NSMenuItem alloc] initWithTitle:Ladybird::string_to_ns_string(name)
                                                                              action:@selector(setNavigatorCompatibilityMode:)
                                                                       keyEquivalent:@""]];
    };
    add_navigator_compatibility_mode("Chrome");
    add_navigator_compatibility_mode("Gecko");
    add_navigator_compatibility_mode("WebKit");

    auto* navigator_compatibility_mode_menu_item = [[NSMenuItem alloc] initWithTitle:@"Navigator Compatibility Mode"
                                                                              action:nil
                                                                       keyEquivalent:@""];
    [navigator_compatibility_mode_menu_item setSubmenu:navigator_compatibility_mode_menu];

    [submenu addItem:navigator_compatibility_mode_menu_item];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Enable Scripting"
                                                action:@selector(toggleScripting:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Block Pop-ups"
                                                action:@selector(togglePopupBlocking:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Enable Same-Origin Policy"
                                                action:@selector(toggleSameOriginPolicy:)
                                         keyEquivalent:@""]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createWindowMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"Window"];

    [NSApp setWindowsMenu:submenu];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createHelpMenu
{
    auto* menu = [[NSMenuItem alloc] init];
    auto* submenu = [[NSMenu alloc] initWithTitle:@"Help"];

    [NSApp setHelpMenu:submenu];

    [menu setSubmenu:submenu];
    return menu;
}

#pragma mark - NSApplicationDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    Tab* tab = nil;

    for (auto const& url : WebView::Application::browser_options().urls) {
        auto activate_tab = tab == nil ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No;

        auto* controller = [self createNewTab:url
                                      fromTab:tab
                                  activateTab:activate_tab];

        tab = (Tab*)[controller window];
    }
}

- (void)applicationWillTerminate:(NSNotification*)notification
{
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return YES;
}

- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    if ([item action] == @selector(setAutoPreferredColorScheme:)) {
        [item setState:(m_preferred_color_scheme == Web::CSS::PreferredColorScheme::Auto) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setDarkPreferredColorScheme:)) {
        [item setState:(m_preferred_color_scheme == Web::CSS::PreferredColorScheme::Dark) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setLightPreferredColorScheme:)) {
        [item setState:(m_preferred_color_scheme == Web::CSS::PreferredColorScheme::Light) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setAutoPreferredContrast:)) {
        [item setState:(m_preferred_contrast == Web::CSS::PreferredContrast::Auto) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setLessPreferredContrast:)) {
        [item setState:(m_preferred_contrast == Web::CSS::PreferredContrast::Less) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setMorePreferredContrast:)) {
        [item setState:(m_preferred_contrast == Web::CSS::PreferredContrast::More) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setNoPreferencePreferredContrast:)) {
        [item setState:(m_preferred_contrast == Web::CSS::PreferredContrast::NoPreference) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setAutoPreferredMotion:)) {
        [item setState:(m_preferred_motion == Web::CSS::PreferredMotion::Auto) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setNoPreferencePreferredMotion:)) {
        [item setState:(m_preferred_motion == Web::CSS::PreferredMotion::NoPreference) ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setReducePreferredMotion:)) {
        [item setState:(m_preferred_motion == Web::CSS::PreferredMotion::Reduce) ? NSControlStateValueOn : NSControlStateValueOff];
    }

    return YES;
}

@end
