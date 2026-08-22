/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibWebView/Application.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/ViewImplementation.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Interface/InfoBar.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Menu.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>
#import <Utilities/Conversions.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static char s_tab_group_observation_context;

@interface ApplicationDelegate ()

@property (nonatomic, strong) NSMutableArray<TabController*>* managed_tabs;
@property (nonatomic, weak) Tab* active_tab;

@property (nonatomic, strong) NSMapTable<TabController*, NSNumber*>* session_window_ids;
@property (nonatomic, strong) NSMapTable<NSWindowTabGroup*, NSNumber*>* tab_group_session_window_ids;
@property (nonatomic, strong) NSHashTable<NSWindowTabGroup*>* observed_tab_groups;
@property (nonatomic, assign) BOOL session_topology_reconciliation_scheduled;
@property (nonatomic, assign) BOOL reconciling_session_topology;

@property (nonatomic, strong) NSMenu* bookmarks_menu;

@property (nonatomic, strong) InfoBar* info_bar;

- (NSMenuItem*)createApplicationMenu;
- (NSMenuItem*)createFileMenu;
- (NSMenuItem*)createEditMenu;
- (NSMenuItem*)createViewMenu;
- (NSMenuItem*)createHistoryMenu;
- (NSMenuItem*)createBookmarksMenu;
- (NSMenuItem*)createInspectMenu;
- (NSMenuItem*)createDebugMenu;
- (NSMenuItem*)createWindowMenu;
- (NSMenuItem*)createHelpMenu;

- (void)scheduleSessionTopologyReconciliation;
- (void)retireSessionWindow:(WebView::SessionWindowId)window_id isPrivate:(WebView::IsPrivate)is_private;

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
        [[NSApp mainMenu] addItem:[self createHistoryMenu]];
        [[NSApp mainMenu] addItem:[self createBookmarksMenu]];
        [[NSApp mainMenu] addItem:[self createInspectMenu]];
        [[NSApp mainMenu] addItem:[self createDebugMenu]];
        [[NSApp mainMenu] addItem:[self createWindowMenu]];
        [[NSApp mainMenu] addItem:[self createHelpMenu]];

        self.managed_tabs = [[NSMutableArray alloc] init];
        self.session_window_ids = [NSMapTable strongToStrongObjectsMapTable];
        self.tab_group_session_window_ids = [NSMapTable weakToStrongObjectsMapTable];
        self.observed_tab_groups = [NSHashTable weakObjectsHashTable];

        // Reduce the tooltip delay, as the default delay feels quite long.
        [[NSUserDefaults standardUserDefaults] setObject:@100 forKey:@"NSInitialToolTipDelay"];
    }

    return self;
}

- (void)dealloc
{
    for (NSWindowTabGroup* tab_group in self.observed_tab_groups) {
        [tab_group removeObserver:self forKeyPath:@"windows" context:&s_tab_group_observation_context];
        [tab_group removeObserver:self forKeyPath:@"selectedWindow" context:&s_tab_group_observation_context];
    }
}

#pragma mark - Public methods

- (nonnull TabController*)createNewTab:(Web::HTML::ActivateTab)activate_tab
                               fromTab:(nullable Tab*)tab
{
    auto is_private = tab ? [tab isPrivate] : WebView::IsPrivate::No;
    auto* controller = [[TabController alloc] init:is_private];

    [self initializeTabController:controller
                      activateTab:activate_tab
                          fromTab:tab];

    return controller;
}

- (TabController*)createNewTab:(Optional<URL::URL> const&)url
                       fromTab:(Tab*)tab
                     isPrivate:(WebView::IsPrivate)is_private
                   activateTab:(Web::HTML::ActivateTab)activate_tab
                   tabLocation:(TabLocation)tab_location
{
    auto* controller = [[TabController alloc] init:is_private];

    [self initializeTabController:controller
                      activateTab:activate_tab
                          fromTab:tab
                      tabLocation:tab_location];

    if (url.has_value()) {
        [controller loadURL:*url];

        if (*url != WebView::Application::settings().new_tab_page_url())
            [controller focusWebView];
    }

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

    [controller focusWebView];

    return controller;
}

- (void)setActiveTab:(Tab*)tab
{
    if (tab == self.activeTab)
        return;

    self.active_tab = tab;

    if (self.info_bar) {
        [self.info_bar tabBecameActive:self.active_tab];
    }

    WebView::Application::the().update_bookmark_action_for_current_web_view();
    WebView::Application::the().update_editing_history_actions();
}

- (Tab*)activeTab
{
    return self.active_tab;
}

- (void)reconcileSessionTopology
{
    if (self.reconciling_session_topology)
        return;
    self.reconciling_session_topology = YES;
    ScopeGuard clear_reconciliation_flag = [&] {
        self.reconciling_session_topology = NO;
    };

    HashTable<WebView::SessionWindowId> previous_normal_window_ids;
    HashTable<WebView::SessionWindowId> previous_private_window_ids;
    auto* controllers_by_window = [NSMapTable<NSWindow*, TabController*> strongToStrongObjectsMapTable];
    auto* representative_controllers = [NSMapTable<NSWindowTabGroup*, TabController*> strongToStrongObjectsMapTable];
    auto* tab_groups = [[NSMutableArray<NSWindowTabGroup*> alloc] init];

    for (TabController* controller in self.managed_tabs) {
        if (auto* window_id = [self.session_window_ids objectForKey:controller]) {
            auto& previous_window_ids = [controller isPrivate] == WebView::IsPrivate::Yes ? previous_private_window_ids : previous_normal_window_ids;
            previous_window_ids.set([window_id longLongValue]);
        }

        auto* window = [controller window];
        if (!window)
            continue;
        [controllers_by_window setObject:controller forKey:window];

        auto* tab_group = [window tabGroup];
        if (tab_group && ![tab_groups containsObject:tab_group]) {
            [tab_groups addObject:tab_group];
            [representative_controllers setObject:controller forKey:tab_group];
        }
    }

    HashTable<WebView::SessionWindowId> assigned_normal_window_ids;
    HashTable<WebView::SessionWindowId> assigned_private_window_ids;
    auto* target_window_ids = [NSMapTable<NSWindowTabGroup*, NSNumber*> strongToStrongObjectsMapTable];

    // Preserve IDs already owned by a native tab group before considering tab-derived fallbacks.
    for (NSWindowTabGroup* tab_group in tab_groups) {
        auto* target_window_id = [self.tab_group_session_window_ids objectForKey:tab_group];
        if (!target_window_id)
            continue;

        auto* representative_controller = [representative_controllers objectForKey:tab_group];
        auto& assigned_window_ids = [representative_controller isPrivate] == WebView::IsPrivate::Yes ? assigned_private_window_ids : assigned_normal_window_ids;
        auto window_id = static_cast<WebView::SessionWindowId>([target_window_id longLongValue]);
        if (assigned_window_ids.contains(window_id))
            continue;

        assigned_window_ids.set(window_id);
        [target_window_ids setObject:target_window_id forKey:tab_group];
    }

    for (NSWindowTabGroup* tab_group in tab_groups) {
        if ([target_window_ids objectForKey:tab_group])
            continue;

        auto* representative_controller = [representative_controllers objectForKey:tab_group];
        auto& assigned_window_ids = [representative_controller isPrivate] == WebView::IsPrivate::Yes ? assigned_private_window_ids : assigned_normal_window_ids;

        for (NSWindow* window in [tab_group windows]) {
            auto* controller = [controllers_by_window objectForKey:window];
            if (!controller)
                continue;
            auto* candidate = [self.session_window_ids objectForKey:controller];
            if (!candidate)
                continue;

            auto candidate_id = static_cast<WebView::SessionWindowId>([candidate longLongValue]);
            if (assigned_window_ids.contains(candidate_id))
                continue;

            assigned_window_ids.set(candidate_id);
            [target_window_ids setObject:candidate forKey:tab_group];
            break;
        }
    }

    struct NewlyOpenedWindow {
        WebView::SessionWindowId window_id;
        WebView::IsPrivate is_private;
    };
    Vector<NewlyOpenedWindow> newly_opened_windows;

    for (NSWindowTabGroup* tab_group in tab_groups) {
        if ([target_window_ids objectForKey:tab_group])
            continue;

        auto is_private = [[representative_controllers objectForKey:tab_group] isPrivate];
        auto result = WebView::Application::session_store(is_private).window_opened();
        if (result.is_error()) {
            dbgln("Unable to register the new window with the session store: {}", result.error());
            for (auto const& window : newly_opened_windows)
                [self retireSessionWindow:window.window_id isPrivate:window.is_private];
            return;
        }

        auto* target_window_id = [NSNumber numberWithLongLong:result.value()];
        newly_opened_windows.append({ .window_id = result.value(), .is_private = is_private });
        auto& assigned_window_ids = is_private == WebView::IsPrivate::Yes ? assigned_private_window_ids : assigned_normal_window_ids;
        assigned_window_ids.set(result.value());
        [target_window_ids setObject:target_window_id forKey:tab_group];
    }

    for (NSWindowTabGroup* tab_group in [self.observed_tab_groups allObjects]) {
        if ([tab_groups containsObject:tab_group])
            continue;
        [tab_group removeObserver:self forKeyPath:@"windows" context:&s_tab_group_observation_context];
        [tab_group removeObserver:self forKeyPath:@"selectedWindow" context:&s_tab_group_observation_context];
        [self.observed_tab_groups removeObject:tab_group];
    }

    for (NSWindowTabGroup* tab_group in tab_groups) {
        if (![self.observed_tab_groups containsObject:tab_group]) {
            [tab_group addObserver:self forKeyPath:@"windows" options:0 context:&s_tab_group_observation_context];
            [tab_group addObserver:self forKeyPath:@"selectedWindow" options:0 context:&s_tab_group_observation_context];
            [self.observed_tab_groups addObject:tab_group];
        }

        auto* target_window_id = [target_window_ids objectForKey:tab_group];
        if (!target_window_id)
            continue;
        [self.tab_group_session_window_ids setObject:target_window_id forKey:tab_group];

        auto window_id = static_cast<WebView::SessionWindowId>([target_window_id longLongValue]);
        auto is_private = WebView::IsPrivate::No;
        Vector<WebView::SessionTabId> tab_order;
        Optional<WebView::SessionTabId> active_tab_id;

        for (NSWindow* window in [tab_group windows]) {
            auto* controller = [controllers_by_window objectForKey:window];
            if (!controller)
                continue;

            is_private = [controller isPrivate];
            auto session_tab_id = [[(Tab*)window web_view] view].session_tab_id();
            if (session_tab_id.has_value()) {
                auto* previous_window_id = [self.session_window_ids objectForKey:controller];
                if (!previous_window_id || ![previous_window_id isEqualToNumber:target_window_id]) {
                    WebView::SessionStore::TabMoved moved {
                        .tab_id = *session_tab_id,
                        .new_window_id = window_id,
                        .ordinal = static_cast<i64>(tab_order.size()),
                    };
                    WebView::Application::session_store(is_private).tab_moved(AK::move(moved));
                }
                tab_order.append(*session_tab_id);
                if ([tab_group selectedWindow] == window)
                    active_tab_id = *session_tab_id;
            }

            [self.session_window_ids setObject:target_window_id forKey:controller];
        }

        WebView::SessionStore::TabOrderChanged changed {
            .window_id = window_id,
            .ordered_tabs = AK::move(tab_order),
        };
        auto& session_store = WebView::Application::session_store(is_private);
        session_store.tab_order_changed(AK::move(changed));
        if (active_tab_id.has_value())
            session_store.active_tab_changed(*active_tab_id);
    }

    for (auto window_id : previous_normal_window_ids) {
        if (!assigned_normal_window_ids.contains(window_id))
            [self retireSessionWindow:window_id isPrivate:WebView::IsPrivate::No];
    }
    for (auto window_id : previous_private_window_ids) {
        if (!assigned_private_window_ids.contains(window_id))
            [self retireSessionWindow:window_id isPrivate:WebView::IsPrivate::Yes];
    }
    for (NSWindowTabGroup* tab_group in [[self.tab_group_session_window_ids keyEnumerator] allObjects]) {
        if (![tab_groups containsObject:tab_group])
            [self.tab_group_session_window_ids removeObjectForKey:tab_group];
    }
}

- (void)scheduleSessionTopologyReconciliation
{
    if (self.session_topology_reconciliation_scheduled)
        return;
    self.session_topology_reconciliation_scheduled = YES;

    __weak ApplicationDelegate* weak_self = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        ApplicationDelegate* strong_self = weak_self;
        if (!strong_self)
            return;
        strong_self.session_topology_reconciliation_scheduled = NO;
        [strong_self reconcileSessionTopology];
    });
}

- (void)retireSessionWindow:(WebView::SessionWindowId)window_id isPrivate:(WebView::IsPrivate)is_private
{
    WebView::Application::session_store(is_private).window_detached(window_id);
}

- (void)observeValueForKeyPath:(NSString*)key_path
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context
{
    if (context == &s_tab_group_observation_context) {
        [self scheduleSessionTopologyReconciliation];
        return;
    }
    [super observeValueForKeyPath:key_path ofObject:object change:change context:context];
}

- (void)removeTab:(TabController*)controller
{
    auto* previous_window_id = [self.session_window_ids objectForKey:controller];
    auto is_private = [controller isPrivate];
    [self.managed_tabs removeObject:controller];
    [self.session_window_ids removeObjectForKey:controller];
    [self reconcileSessionTopology];

    if (!previous_window_id)
        return;
    for (TabController* remaining_controller in self.managed_tabs) {
        if ([remaining_controller isPrivate] == is_private && [[self.session_window_ids objectForKey:remaining_controller] isEqualToNumber:previous_window_id])
            return;
    }
    [self retireSessionWindow:[previous_window_id longLongValue] isPrivate:is_private];
}

- (NSUInteger)tabCount
{
    return self.managed_tabs.count;
}

- (void)restartPrivateBrowsingSession
{
    for (TabController* controller in [self.managed_tabs copy]) {
        if ([controller isPrivate] == WebView::IsPrivate::Yes)
            [[controller window] close];
    }

    WebView::Application::the().reset_private_browsing_session();
    [self openNewWindow:WebView::IsPrivate::Yes];
}

- (void)rebuildBookmarksMenu
{
    Ladybird::repopulate_application_menu(self.bookmarks_menu, WebView::Application::the().bookmarks_menu());

    for (TabController* controller in self.managed_tabs) {
        auto* tab = (Tab*)[controller window];
        [tab rebuildBookmarksBar];
    }
}

- (void)onDevtoolsEnabled
{
    if (!self.info_bar) {
        self.info_bar = [[InfoBar alloc] init];
    }

    auto message = MUST(String::formatted("DevTools is enabled on port {}", WebView::Application::browser_options().devtools_port));

    [self.info_bar showWithMessage:Ladybird::string_to_ns_string(message)
        actionButtonTitle:@"Open Client"
        actionButtonClicked:^{
            if (auto result = WebView::Application::the().launch_devtools_client(); result.is_error())
                WebView::Application::the().display_error_dialog(MUST(String::formatted("Unable to launch the DevTools client: {}", result.error())));
        }
        dismissButtonTitle:@"Disable"
        dismissButtonClicked:^{
            MUST(WebView::Application::the().toggle_devtools_enabled());
        }
        activeTab:self.active_tab];
}

- (void)onDevtoolsDisabled
{
    if (self.info_bar) {
        [self.info_bar hide];
        self.info_bar = nil;
    }
}

#pragma mark - Private methods

- (void)openLocation:(id)sender
{
    auto* current_tab = [NSApp keyWindow];

    if (![current_tab isKindOfClass:[Tab class]]) {
        return;
    }

    auto* controller = (TabController*)[current_tab windowController];
    [controller focusLocationToolbarItem];
}

- (void)createNewWindow:(id)sender
{
    [self openNewWindow:WebView::IsPrivate::No];
}

- (void)createNewPrivateWindow:(id)sender
{
    [self openNewWindow:WebView::IsPrivate::Yes];
}

- (void)openNewWindow:(WebView::IsPrivate)is_private
{
    // FIXME: Create a new tab page specific to private windows.
    [self createNewTab:WebView::Application::settings().new_tab_page_url()
               fromTab:nil
             isPrivate:is_private
           activateTab:Web::HTML::ActivateTab::Yes
           tabLocation:TabLocation::end()];
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
    [self initializeTabController:controller
                      activateTab:activate_tab
                          fromTab:tab
                      tabLocation:TabLocation::end()];
}

- (void)initializeTabController:(TabController*)controller
                    activateTab:(Web::HTML::ActivateTab)activate_tab
                        fromTab:(nullable Tab*)tab
                    tabLocation:(TabLocation)tab_location
{
    Optional<i64> insertion_index;
    NSWindowTabGroup* tab_group = nil;

    auto* tab_for_location = tab_location.is_after_tab() ? tab_location.tab() : tab;
    if (tab_for_location && [tab_for_location isPrivate] != [controller isPrivate])
        tab_for_location = nil;

    if (tab_for_location) {
        tab_group = [tab_for_location tabGroup];

        if (tab_location.is_after_tab()) {
            auto* windows = [tab_group windows];
            auto tab_index = [windows indexOfObject:tab_for_location];
            if (tab_index != NSNotFound)
                insertion_index = static_cast<i64>(tab_index + 1);
        } else if (tab_location.is_at_index()) {
            insertion_index = clamp(tab_location.index(), 0, static_cast<i64>([[tab_group windows] count]));
        }
    }

    [controller showWindow:nil];

    if (tab_for_location) {
        if (insertion_index.has_value())
            [tab_group insertWindow:controller.window atIndex:static_cast<NSInteger>(*insertion_index)];
        else
            [tab_group addWindow:controller.window];

        // FIXME: Can we create the tabbed window above without it becoming active in the first place?
        if (activate_tab == Web::HTML::ActivateTab::No) {
            [tab_for_location orderFront:nil];
        }
    }

    if (activate_tab == Web::HTML::ActivateTab::Yes) {
        [[controller window] orderFrontRegardless];
        [controller focusLocationToolbarItem];
    }

    [self.managed_tabs addObject:controller];

    [self reconcileSessionTopology];

    Optional<WebView::SessionWindowId> session_window_id;
    if (auto* registered_window_id = [self.session_window_ids objectForKey:controller])
        session_window_id = static_cast<WebView::SessionWindowId>([registered_window_id longLongValue]);

    Optional<i64> session_insertion_index;
    auto* native_tab_group = [[controller window] tabGroup];
    if (auto native_tab_index = [[native_tab_group windows] indexOfObject:[controller window]]; native_tab_index != NSNotFound)
        session_insertion_index = static_cast<i64>(native_tab_index);

    WebView::SessionStore::TabOpened opened {
        .window_id = session_window_id,
        .initial_url = {},
        .insertion_index = session_insertion_index,
        .is_active = activate_tab == Web::HTML::ActivateTab::Yes ? WebView::SessionStore::IsActive::Yes : WebView::SessionStore::IsActive::No,
    };
    auto session_tab_id = WebView::Application::session_store([controller isPrivate]).tab_opened(move(opened));
    if (session_tab_id.is_error())
        dbgln("Unable to register the new tab with the session store: {}", session_tab_id.error());
    else
        [[(Tab*)[controller window] web_view] view].set_session_tab_id(session_tab_id.value());
}

- (void)closeCurrentTab:(id)sender
{
    auto* current_window = [NSApp keyWindow];
    [current_window performClose:self];
}

- (void)duplicateCurrentTab:(id)sender
{
    auto* key_window = [NSApp keyWindow];
    if (![key_window isKindOfClass:[Tab class]])
        return;
    auto* source_tab = (Tab*)key_window;

    auto& source_view = [[source_tab web_view] view];
    auto history = source_view.session_history_snapshot();
    auto source_url = source_view.url();

    auto* controller = [self createNewTab:Optional<URL::URL> { }
                                  fromTab:source_tab
                                isPrivate:[source_tab isPrivate]
                              activateTab:Web::HTML::ActivateTab::Yes
                              tabLocation:TabLocation::after_tab(source_tab)];
    auto& duplicate_view = [[(Tab*)[controller window] web_view] view];

    if (!history.has_value() || duplicate_view.restore_session_history_from_snapshot(history.release_value()).is_error())
        [controller loadURL:source_url];
    [controller focusWebView];
}

- (WebView::IsPrivate)keyWindowPrivacy
{
    if (auto* key_window = [NSApp keyWindow]; [key_window isKindOfClass:[Tab class]])
        return [(Tab*)key_window isPrivate];
    return WebView::IsPrivate::No;
}

- (void)reopenClosedTab:(id)sender
{
    auto is_private = [self keyWindowPrivacy];
    [self reconcileSessionTopology];
    auto closed_unit = WebView::Application::session_store(is_private).take_most_recently_closed();
    if (closed_unit.is_error()) {
        dbgln("Unable to reopen the most recently closed session unit: {}", closed_unit.error());
        return;
    }
    if (!closed_unit.value().has_value())
        return;

    auto unit = closed_unit.value().release_value();
    Tab* destination_tab = nil;
    bool restoring_to_source_window = false;
    if (!unit.was_window && unit.source_window_id.has_value()) {
        for (TabController* controller in self.managed_tabs) {
            auto* session_window_id = [self.session_window_ids objectForKey:controller];
            if ([controller isPrivate] != is_private || !session_window_id || [session_window_id longLongValue] != *unit.source_window_id)
                continue;
            destination_tab = (Tab*)[controller window];
            restoring_to_source_window = true;
            break;
        }
    }
    if (!unit.was_window && !destination_tab) {
        auto* active_tab = [self activeTab];
        if (active_tab && [active_tab isPrivate] == is_private && [self.managed_tabs containsObject:[active_tab windowController]])
            destination_tab = active_tab;
    }

    Optional<i64> next_insertion_index;
    if (restoring_to_source_window && unit.source_tab_index.has_value())
        next_insertion_index = clamp(*unit.source_tab_index, 0, static_cast<i64>([[destination_tab tabGroup] windows].count));

    Tab* first_tab = nil;
    TabController* active_tab_controller = nil;

    i64 restored_tab_index = 0;
    for (auto& closed_tab : unit.tabs) {
        auto* tab_for_group = first_tab ? first_tab : destination_tab;
        auto tab_location = next_insertion_index.has_value() ? TabLocation::at_index(*next_insertion_index) : TabLocation::end();
        auto* controller = [self createNewTab:Optional<URL::URL> { }
                                      fromTab:tab_for_group
                                    isPrivate:is_private
                                  activateTab:Web::HTML::ActivateTab::No
                                  tabLocation:tab_location];
        auto* tab = (Tab*)[controller window];
        if (!first_tab)
            first_tab = tab;
        if (next_insertion_index.has_value())
            ++*next_insertion_index;
        if (restored_tab_index == unit.active_tab_index)
            active_tab_controller = controller;

        auto& view = [[tab web_view] view];
        if (!closed_tab.history.has_value() || view.restore_session_history_from_snapshot(closed_tab.history.release_value()).is_error())
            [controller loadURL:closed_tab.active_url];
        ++restored_tab_index;
    }

    if (active_tab_controller)
        [[active_tab_controller window] makeKeyAndOrderFront:nil];
    [self reconcileSessionTopology];
}

- (NSMenuItem*)createApplicationMenu
{
    auto* menu = [[NSMenuItem alloc] init];

    auto* process_name = [[NSProcessInfo processInfo] processName];
    auto* submenu = [[NSMenu alloc] initWithTitle:process_name];

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().open_about_page_action())];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().open_settings_page_action())];
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

    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"New Window"
                                                action:@selector(createNewWindow:)
                                         keyEquivalent:@"n"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"New Private Window"
                                                action:@selector(createNewPrivateWindow:)
                                         keyEquivalent:@"N"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"New Tab"
                                                action:@selector(createNewTab:)
                                         keyEquivalent:@"t"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Duplicate Tab"
                                                action:@selector(duplicateCurrentTab:)
                                         keyEquivalent:@""]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Reopen Closed Tab"
                                                action:@selector(reopenClosedTab:)
                                         keyEquivalent:@"T"]];
    [submenu addItem:[[NSMenuItem alloc] initWithTitle:@"Close Tab"
                                                action:@selector(closeCurrentTab:)
                                         keyEquivalent:@"w"]];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().open_downloads_page_action())];
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

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().undo_action())];
    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().redo_action())];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().cut_selection_action())];
    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().copy_selection_action())];
    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().paste_action())];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().select_all_action())];
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

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().reload_action())];
    [submenu addItem:[NSMenuItem separatorItem]];

    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().zoom_menu())];
    [submenu addItem:[NSMenuItem separatorItem]];
    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().color_scheme_menu())];
    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().contrast_menu())];
    [submenu addItem:Ladybird::create_application_menu_item(WebView::Application::the().motion_menu())];
    [submenu addItem:[NSMenuItem separatorItem]];

    [menu setSubmenu:submenu];
    return menu;
}

- (NSMenuItem*)createHistoryMenu
{
    return Ladybird::create_application_menu_item(WebView::Application::the().history_menu());
}

- (NSMenuItem*)createBookmarksMenu
{
    auto* menu = Ladybird::create_application_menu_item(WebView::Application::the().bookmarks_menu());
    self.bookmarks_menu = [menu submenu];
    return menu;
}

- (NSMenuItem*)createInspectMenu
{
    return Ladybird::create_application_menu_item(WebView::Application::the().inspect_menu());
}

- (NSMenuItem*)createDebugMenu
{
    return Ladybird::create_application_menu_item(WebView::Application::the().debug_menu());
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
    auto const& browser_options = WebView::Application::browser_options();

    if (browser_options.devtools_port.has_value())
        [self onDevtoolsEnabled];

    Tab* tab = nil;

    for (auto const& url : browser_options.urls) {
        auto activate_tab = tab == nil ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No;

        auto* controller = [self createNewTab:url
                                      fromTab:tab
                                    isPrivate:WebView::IsPrivate::No
                                  activateTab:activate_tab
                                  tabLocation:TabLocation::end()];

        tab = (Tab*)[controller window];
    }
}

- (void)applicationWillTerminate:(NSNotification*)notification
{
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return [(Application*)sender confirmStopActiveDownloads];
}

- (void)applicationDidChangeScreenParameters:(NSNotification*)notification
{
    for (TabController* controller in self.managed_tabs) {
        auto* tab = (Tab*)[controller window];
        [[tab web_view] handleDisplayRefreshRateChange];
    }
}

- (BOOL)validateMenuItem:(NSMenuItem*)menu
{
    SEL action = [menu action];

    if (action == @selector(closeCurrentTab:) || action == @selector(duplicateCurrentTab:)) {
        return [[NSApp keyWindow] isKindOfClass:[Tab class]];
    }
    if (action == @selector(reopenClosedTab:)) {
        return WebView::Application::session_store([self keyWindowPrivacy]).has_closed_units();
    }

    return YES;
}

@end
