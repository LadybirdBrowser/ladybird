/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>

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

@interface DeallocGuard : NSObject
{
    Function<void()> m_on_deallocation;
}
@end

@implementation DeallocGuard

- (instancetype)init:(Function<void()>)on_deallocation
{
    if (self = [super init]) {
        m_on_deallocation = move(on_deallocation);
    }

    return self;
}

- (void)dealloc
{
    if (m_on_deallocation)
        m_on_deallocation();
}

@end

namespace Ladybird {

static char PROPERTIES_KEY = 0;

template<typename T>
static void set_properties(id control, T const& item)
{
    if (item.properties().is_empty())
        return;

    auto* properties = [[NSMutableDictionary alloc] init];

    for (auto const& [key, value] : item.properties())
        [properties setObject:string_to_ns_string(value) forKey:string_to_ns_string(key)];

    objc_setAssociatedObject(control, &PROPERTIES_KEY, properties, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void add_control_properties(id control, WebView::Action const& action)
{
    set_properties(control, action);
}

void add_control_properties(id control, WebView::Menu const& menu)
{
    set_properties(control, menu);
}

NSString* get_control_property(id control, NSString* key)
{
    NSDictionary* properties = objc_getAssociatedObject(control, &PROPERTIES_KEY);

    if (properties)
        return [properties objectForKey:key];

    return nil;
}

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

    virtual void on_engaged_state_changed(WebView::Action& action) override
    {
        switch (action.id()) {
        case WebView::ActionID::ToggleBookmark:
        case WebView::ActionID::ToggleBookmarkViaToolbar:
            set_control_image(m_control, action.engaged() ? @"star.fill" : @"star");
            break;
        default:
            break;
        }
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

static NSImage* image_from_base64_png(StringView favicon_base64_png)
{
    static constexpr CGFloat const MENU_ICON_SIZE = 16;

    auto decoded = decode_base64(favicon_base64_png);
    if (decoded.is_error())
        return nil;

    auto* data = [NSData dataWithBytes:decoded.value().data()
                                length:decoded.value().size()];

    auto* image = [[NSImage alloc] initWithData:data];
    [image setSize:NSMakeSize(MENU_ICON_SIZE, MENU_ICON_SIZE)];

    return image;
}

static void initialize_native_icon(WebView::Action& action, id control)
{
    switch (action.id()) {
    case WebView::ActionID::NavigateBack:
        set_control_image(control, @"chevron.left");
        [control setKeyEquivalent:@"["];
        break;
    case WebView::ActionID::NavigateForward:
        set_control_image(control, @"chevron.right");
        [control setKeyEquivalent:@"]"];
        break;
    case WebView::ActionID::Reload:
        set_control_image(control, @"arrow.clockwise");
        [control setKeyEquivalent:@"r"];
        break;

    case WebView::ActionID::CopySelection:
        set_control_image(control, @"document.on.document");
        [control setKeyEquivalent:@"c"];
        break;
    case WebView::ActionID::Paste:
        set_control_image(control, @"document.on.clipboard");
        [control setKeyEquivalent:@"v"];
        break;
    case WebView::ActionID::SelectAll:
        set_control_image(control, @"character.textbox");
        [control setKeyEquivalent:@"a"];
        break;

    case WebView::ActionID::SearchSelectedText:
        set_control_image(control, @"magnifyingglass");
        break;

    case WebView::ActionID::ManageBookmarks:
        set_control_image(control, @"bookmark");
        break;
    case WebView::ActionID::ToggleBookmark:
        [control setKeyEquivalent:@"d"];
        break;
    case WebView::ActionID::ToggleBookmarksBar:
        set_control_image(control, @"line.horizontal.star.fill.line.horizontal");
        [control setKeyEquivalent:@"B"];
        break;
    case WebView::ActionID::BookmarkItem:
        if (auto icon = action.base64_png_icon(); icon.has_value())
            [control setImage:image_from_base64_png(*icon)];
        else
            set_control_image(control, @"globe");
        break;

    case WebView::ActionID::OpenAboutPage:
        set_control_image(control, @"info.circle");
        break;
    case WebView::ActionID::OpenProcessesPage:
        set_control_image(control, @"gearshape.2");
        [control setKeyEquivalent:@"M"];
        break;
    case WebView::ActionID::OpenSettingsPage:
        set_control_image(control, @"gearshape");
        [control setKeyEquivalent:@","];
        break;
    case WebView::ActionID::ToggleDevTools:
        set_control_image(control, @"chevron.left.chevron.right");
        [control setKeyEquivalent:@"I"];
        break;
    case WebView::ActionID::ViewSource:
        set_control_image(control, @"text.document");
        [control setKeyEquivalent:@"u"];
        break;

    case WebView::ActionID::TakeVisibleScreenshot:
    case WebView::ActionID::TakeFullScreenshot:
        set_control_image(control, @"photo");
        break;

    case WebView::ActionID::OpenInNewTab:
        set_control_image(control, @"plus.square.on.square");
        break;
    case WebView::ActionID::CopyURL:
        set_control_image(control, @"document.on.document");
        break;

    case WebView::ActionID::OpenImage:
        set_control_image(control, @"photo");
        break;
    case WebView::ActionID::SaveImage:
        set_control_image(control, @"square.and.arrow.down");
        break;
    case WebView::ActionID::CopyImage:
        set_control_image(control, @"document.on.document");
        break;

    case WebView::ActionID::OpenAudio:
        set_control_image(control, @"speaker.wave.1");
        break;
    case WebView::ActionID::OpenVideo:
        set_control_image(control, @"video");
        break;
    case WebView::ActionID::PlayMedia:
        set_control_image(control, @"play");
        break;
    case WebView::ActionID::PauseMedia:
        set_control_image(control, @"pause");
        break;
    case WebView::ActionID::MuteMedia:
        set_control_image(control, @"speaker.slash");
        break;
    case WebView::ActionID::UnmuteMedia:
        set_control_image(control, @"speaker.wave.2");
        break;
    case WebView::ActionID::ShowControls:
        set_control_image(control, @"eye");
        break;
    case WebView::ActionID::HideControls:
        set_control_image(control, @"eye.slash");
        break;
    case WebView::ActionID::ToggleMediaLoopState:
        set_control_image(control, @"arrow.clockwise");
        break;
    case WebView::ActionID::EnterFullscreen:
        set_control_image(control, @"arrow.up.left.and.arrow.down.right");
        break;
    case WebView::ActionID::ExitFullscreen:
        set_control_image(control, @"arrow.down.right.and.arrow.up.left");
        break;

    case WebView::ActionID::ZoomIn:
        set_control_image(control, @"plus.magnifyingglass");
        [control setKeyEquivalent:@"+"];
        break;
    case WebView::ActionID::ZoomOut:
        set_control_image(control, @"minus.magnifyingglass");
        [control setKeyEquivalent:@"-"];
        break;
    case WebView::ActionID::ResetZoom:
        set_control_image(control, @"1.magnifyingglass");
        [control setKeyEquivalent:@"0"];
        break;

    default:
        break;
    }
}

static void initialize_native_control(WebView::Action& action, id control)
{
    initialize_native_icon(action, control);

    auto observer = ActionObserver::create(action, control);

    auto* guard = [[DeallocGuard alloc] init:[action = action.make_weak_ptr(), observer = observer.ptr()]() {
        if (action)
            action->remove_observer(*observer);
    }];

    static char guard_key = 0;
    objc_setAssociatedObject(control, &guard_key, guard, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    action.add_observer(move(observer));
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
                set_properties(application_submenu, *submenu);
                add_items_to_menu(application_submenu, submenu->items());

                auto* item = [[NSMenuItem alloc] initWithTitle:string_to_ns_string(submenu->title())
                                                        action:nil
                                                 keyEquivalent:@""];
                [item setSubmenu:application_submenu];

                if (submenu->render_group_icon())
                    set_control_image(item, @"folder");

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
    set_properties(application_menu, menu);
    add_items_to_menu(application_menu, menu.items());
    return application_menu;
}

void repopulate_application_menu(NSMenu* menu, WebView::Menu& source)
{
    [menu removeAllItems];
    add_items_to_menu(menu, source.items());
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
    set_properties(item, action);
    return item;
}

NSButton* create_application_button(WebView::Action& action)
{
    auto* button = [[NSButton alloc] init];
    initialize_native_control(action, button);
    set_properties(button, action);
    return button;
}

NSImageView* create_application_icon(WebView::Action& action)
{
    auto* icon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    initialize_native_icon(action, icon);
    return icon;
}

void set_control_image(id control, NSString* image)
{
    // System symbols are distributed with the San Fransisco (SF) Symbols font. To see all SF Symbols and their names,
    // you will have to install the SF Symbols app: https://developer.apple.com/sf-symbols/
    auto set_image = [&]() {
        [control setImage:[NSImage imageWithSystemSymbolName:image accessibilityDescription:@""]];
    };

    if (@available(macOS 26, *)) {
        set_image();
    } else {
        if ([control isKindOfClass:[NSButton class]])
            set_image();
    }
}

}
