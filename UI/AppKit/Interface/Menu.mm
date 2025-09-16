/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/Event.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Menu.h>
#import <Utilities/Conversions.h>
#import <objc/runtime.h>

@interface ActionExecutor : NSObject
{
    WeakPtr<WebView::Action> m_action;
}
@end

@implementation ActionExecutor

+ (instancetype)attachToNativeControl:(WebView::Action const&)action
                              control:(id)control
{
    auto* executor = [[ActionExecutor alloc] init];
    [control setAction:@selector(execute:)];
    [control setTarget:executor];

    static char executor_key = 0;
    objc_setAssociatedObject(control, &executor_key, executor, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    executor->m_action = action.make_weak_ptr();
    return executor;
}

- (void)execute:(id)sender
{
    auto action = m_action.strong_ref();
    if (!action)
        return;

    if (![[[NSApp keyWindow] firstResponder] isKindOfClass:[LadybirdWebView class]]) {
        switch (action->id()) {
        case WebView::ActionID::CopySelection:
            [NSApp sendAction:@selector(copy:) to:nil from:sender];
            return;
        case WebView::ActionID::Paste:
            [NSApp sendAction:@selector(paste:) to:nil from:sender];
            return;
        case WebView::ActionID::SelectAll:
            [NSApp sendAction:@selector(selectAll:) to:nil from:sender];
            return;
        default:
            break;
        }
    }

    if (action->is_checkable())
        action->set_checked(!action->checked());
    action->activate();
}

@end

namespace Ladybird {

class ActionObserver final : public WebView::Action::Observer {
public:
    static NonnullOwnPtr<ActionObserver> create(WebView::Action& action, id control)
    {
        return adopt_own(*new ActionObserver(action, control));
    }

    virtual void on_text_changed(WebView::Action& action) override
    {
        if ([m_control isKindOfClass:[NSButton class]] && [m_control image] != nil)
            [m_control setToolTip:string_to_ns_string(action.text())];
        else
            [m_control setTitle:string_to_ns_string(action.text())];
    }

    virtual void on_tooltip_changed(WebView::Action& action) override
    {
        [m_control setToolTip:string_to_ns_string(action.tooltip())];
    }

    virtual void on_enabled_state_changed(WebView::Action& action) override
    {
        [m_control setEnabled:action.enabled()];
    }

    virtual void on_visible_state_changed(WebView::Action& action) override
    {
        [m_control setHidden:!action.visible()];

        if ([m_control isKindOfClass:[NSButton class]])
            [m_control setBordered:action.visible()];
    }

    virtual void on_checked_state_changed(WebView::Action& action) override
    {
        [m_control setState:action.checked() ? NSControlStateValueOn : NSControlStateValueOff];
    }

private:
    ActionObserver(WebView::Action& action, id control)
        : m_control(control)
    {
        [ActionExecutor attachToNativeControl:action control:control];
    }

    __weak id m_control { nil };
};

static void initialize_native_control(WebView::Action& action, id control)
{
    switch (action.id()) {
    case WebView::ActionID::NavigateBack:
        [control setKeyEquivalent:@"["];
        break;
    case WebView::ActionID::NavigateForward:
        [control setKeyEquivalent:@"]"];
        break;
    case WebView::ActionID::Reload:
        [control setKeyEquivalent:@"r"];
        break;

    case WebView::ActionID::CopySelection:
        [control setKeyEquivalent:@"c"];
        break;
    case WebView::ActionID::Paste:
        [control setKeyEquivalent:@"v"];
        break;
    case WebView::ActionID::SelectAll:
        [control setKeyEquivalent:@"a"];
        break;

    case WebView::ActionID::ViewSource:
        [control setKeyEquivalent:@"u"];
        break;

    case WebView::ActionID::ZoomIn:
        [control setKeyEquivalent:@"+"];
        break;
    case WebView::ActionID::ZoomOut:
        [control setKeyEquivalent:@"-"];
        break;
    case WebView::ActionID::ResetZoom:
        [control setKeyEquivalent:@"0"];
        break;

    default:
        break;
    }

    action.add_observer(ActionObserver::create(action, control));
}

static void add_items_to_menu(NSMenu* menu, Span<WebView::Menu::MenuItem> menu_items)
{
    for (auto& menu_item : menu_items) {
        menu_item.visit(
            [&](NonnullRefPtr<WebView::Action>& action) {
                [menu addItem:create_application_menu_item(action)];
            },
            [&](NonnullRefPtr<WebView::Menu> const& submenu) {
                auto* application_submenu = [[NSMenu alloc] init];
                add_items_to_menu(application_submenu, submenu->items());

                auto* item = [[NSMenuItem alloc] initWithTitle:string_to_ns_string(submenu->title())
                                                        action:nil
                                                 keyEquivalent:@""];
                [item setSubmenu:application_submenu];

                [menu addItem:item];
            },
            [&](WebView::Separator) {
                [menu addItem:[NSMenuItem separatorItem]];
            });
    }
}

NSMenu* create_application_menu(WebView::Menu& menu)
{
    auto* application_menu = [[NSMenu alloc] initWithTitle:string_to_ns_string(menu.title())];
    add_items_to_menu(application_menu, menu.items());
    return application_menu;
}

NSMenu* create_context_menu(LadybirdWebView* view, WebView::Menu& menu)
{
    auto* application_menu = create_application_menu(menu);

    __weak LadybirdWebView* weak_view = view;
    __weak NSMenu* weak_application_menu = application_menu;

    menu.on_activation = [weak_view, weak_application_menu](Gfx::IntPoint position) {
        LadybirdWebView* view = weak_view;
        NSMenu* application_menu = weak_application_menu;

        if (view && application_menu) {
            auto* event = create_context_menu_mouse_event(view, position);
            [NSMenu popUpContextMenu:application_menu withEvent:event forView:view];
        }
    };

    return application_menu;
}

NSMenuItem* create_application_menu_item(WebView::Action& action)
{
    auto* item = [[NSMenuItem alloc] init];
    initialize_native_control(action, item);
    return item;
}

NSButton* create_application_button(WebView::Action& action, NSImageName image)
{
    auto* button = [[NSButton alloc] init];
    if (image)
        [button setImage:[NSImage imageNamed:image]];

    initialize_native_control(action, button);
    return button;
}

}
