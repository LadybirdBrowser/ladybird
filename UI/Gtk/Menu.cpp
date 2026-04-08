/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Gtk/GLibPtr.h>
#include <UI/Gtk/Menu.h>
#include <UI/Gtk/WebContentView.h>

namespace Ladybird {

class ActionObserver final : public WebView::Action::Observer {
public:
    explicit ActionObserver(GSimpleAction* gaction)
        : m_gaction(GObjectPtr<GSimpleAction> { G_SIMPLE_ACTION(g_object_ref(gaction)) })
    {
    }

    void on_enabled_state_changed(WebView::Action& action) override
    {
        if (m_gaction.ptr())
            g_simple_action_set_enabled(m_gaction, action.enabled());
    }

    void on_checked_state_changed(WebView::Action& action) override
    {
        if (m_gaction.ptr() && g_action_get_state_type(G_ACTION(m_gaction.ptr())))
            g_simple_action_set_state(m_gaction, g_variant_new_boolean(action.checked()));
    }

private:
    GObjectPtr<GSimpleAction> m_gaction;
};

static void set_menu_item_icon_name(GMenuItem* item, char const* icon_name)
{
    if (!icon_name)
        return;

    GObjectPtr icon { g_themed_icon_new(icon_name) };
    g_menu_item_set_icon(item, G_ICON(icon.ptr()));
}

static void set_menu_item_accel(GMenuItem* item, char const* accel)
{
    if (!accel)
        return;
    g_menu_item_set_attribute(item, "accel", "s", accel);
}

static char const* primary_accelerator_for_action(WebView::ActionID id)
{
    switch (id) {
    case WebView::ActionID::NavigateBack:
        return "<Alt>Left";
    case WebView::ActionID::NavigateForward:
        return "<Alt>Right";
    case WebView::ActionID::Reload:
        return "<Ctrl>r";
    case WebView::ActionID::CopySelection:
        return "<Ctrl>c";
    case WebView::ActionID::Paste:
        return "<Ctrl>v";
    case WebView::ActionID::SelectAll:
        return "<Ctrl>a";
    case WebView::ActionID::ToggleBookmark:
        return "<Ctrl>d";
    case WebView::ActionID::ToggleBookmarksBar:
        return "<Ctrl><Shift>b";
    case WebView::ActionID::OpenProcessesPage:
        return "<Ctrl><Shift>m";
    case WebView::ActionID::OpenSettingsPage:
        return "<Ctrl>comma";
    case WebView::ActionID::ToggleDevTools:
        return "<Ctrl><Shift>i";
    case WebView::ActionID::ViewSource:
        return "<Ctrl>u";
    case WebView::ActionID::ZoomIn:
        return "<Ctrl>equal";
    case WebView::ActionID::ZoomOut:
        return "<Ctrl>minus";
    case WebView::ActionID::ResetZoom:
    case WebView::ActionID::ResetZoomViaToolbar:
        return "<Ctrl>0";
    case WebView::ActionID::CollectGarbage:
        return "<Ctrl><Shift>g";
    default:
        return nullptr;
    }
}

static void initialize_native_control(WebView::Action& action, GSimpleAction* gaction, GMenuItem* menu_item)
{
    if (gaction)
        g_simple_action_set_enabled(gaction, action.enabled());

    auto set_icon = [&](char const* icon_name) {
        if (menu_item)
            set_menu_item_icon_name(menu_item, icon_name);
    };
    auto set_accel = [&](char const* accel) {
        if (menu_item)
            set_menu_item_accel(menu_item, accel);
    };

    switch (action.id()) {
    case WebView::ActionID::NavigateBack:
        set_icon("go-previous-symbolic");
        set_accel("<Alt>Left");
        break;
    case WebView::ActionID::NavigateForward:
        set_icon("go-next-symbolic");
        set_accel("<Alt>Right");
        break;
    case WebView::ActionID::Reload:
        set_icon("view-refresh-symbolic");
        set_accel("<Ctrl>r");
        break;

    case WebView::ActionID::CopySelection:
        set_icon("edit-copy-symbolic");
        set_accel("<Ctrl>c");
        break;
    case WebView::ActionID::Paste:
        set_icon("edit-paste-symbolic");
        set_accel("<Ctrl>v");
        break;
    case WebView::ActionID::SelectAll:
        set_icon("edit-select-all-symbolic");
        set_accel("<Ctrl>a");
        break;

    case WebView::ActionID::SearchSelectedText:
        set_icon("edit-find-symbolic");
        break;

    case WebView::ActionID::TakeVisibleScreenshot:
    case WebView::ActionID::TakeFullScreenshot:
        set_icon("image-x-generic-symbolic");
        break;

    case WebView::ActionID::ToggleBookmark:
    case WebView::ActionID::ToggleBookmarkViaToolbar:
        set_icon(action.engaged() ? "starred-symbolic" : "non-starred-symbolic");
        set_accel("<Ctrl>d");
        break;
    case WebView::ActionID::ToggleBookmarksBar:
        set_icon("user-bookmarks-symbolic");
        set_accel("<Ctrl><Shift>b");
        break;
    case WebView::ActionID::BookmarkItem:
        set_icon("globe-symbolic");
        break;

    case WebView::ActionID::OpenAboutPage:
        set_icon("help-about-symbolic");
        break;
    case WebView::ActionID::OpenProcessesPage:
        set_icon("utilities-system-monitor-symbolic");
        set_accel("<Ctrl><Shift>m");
        break;
    case WebView::ActionID::OpenSettingsPage:
        set_icon("emblem-system-symbolic");
        set_accel("<Ctrl>comma");
        break;
    case WebView::ActionID::ToggleDevTools:
    case WebView::ActionID::DumpDOMTree:
        set_icon("applications-engineering-symbolic");
        set_accel("<Ctrl><Shift>i");
        break;
    case WebView::ActionID::ViewSource:
        set_icon("text-html-symbolic");
        set_accel("<Ctrl>u");
        break;

    case WebView::ActionID::OpenInNewTab:
        set_icon("tab-new-symbolic");
        break;
    case WebView::ActionID::CopyURL:
        set_icon("edit-copy-symbolic");
        break;

    case WebView::ActionID::OpenImage:
        set_icon("image-x-generic-symbolic");
        break;
    case WebView::ActionID::SaveImage:
        set_icon("download-symbolic");
        break;
    case WebView::ActionID::CopyImage:
        set_icon("edit-copy-symbolic");
        break;

    case WebView::ActionID::OpenAudio:
        set_icon("audio-x-generic-symbolic");
        break;
    case WebView::ActionID::OpenVideo:
        set_icon("video-x-generic-symbolic");
        break;
    case WebView::ActionID::PlayMedia:
        set_icon("media-playback-start-symbolic");
        break;
    case WebView::ActionID::PauseMedia:
        set_icon("media-playback-pause-symbolic");
        break;
    case WebView::ActionID::MuteMedia:
        set_icon("audio-volume-muted-symbolic");
        break;
    case WebView::ActionID::UnmuteMedia:
        set_icon("audio-volume-high-symbolic");
        break;
    case WebView::ActionID::ShowControls:
        set_icon("view-visible-symbolic");
        break;
    case WebView::ActionID::HideControls:
        set_icon("view-hidden-symbolic");
        break;
    case WebView::ActionID::ToggleMediaLoopState:
        set_icon("view-refresh-symbolic");
        break;
    case WebView::ActionID::EnterFullscreen:
    case WebView::ActionID::ExitFullscreen:
        set_icon("view-fullscreen-symbolic");
        break;

    case WebView::ActionID::ZoomIn:
        set_icon("zoom-in-symbolic");
        set_accel("<Ctrl>equal");
        break;
    case WebView::ActionID::ZoomOut:
        set_icon("zoom-out-symbolic");
        set_accel("<Ctrl>minus");
        break;
    case WebView::ActionID::ResetZoom:
    case WebView::ActionID::ResetZoomViaToolbar:
        set_icon("zoom-original-symbolic");
        set_accel("<Ctrl>0");
        break;

    case WebView::ActionID::DumpSessionHistoryTree:
        set_icon("document-open-recent-symbolic");
        break;
    case WebView::ActionID::DumpLayoutTree:
    case WebView::ActionID::DumpPaintTree:
    case WebView::ActionID::DumpDisplayList:
        set_icon("view-list-symbolic");
        break;
    case WebView::ActionID::DumpStackingContextTree:
        set_icon("view-grid-symbolic");
        break;
    case WebView::ActionID::DumpStyleSheets:
    case WebView::ActionID::DumpStyles:
        set_icon("text-x-css-symbolic");
        break;
    case WebView::ActionID::DumpCSSErrors:
        set_icon("dialog-error-symbolic");
        break;
    case WebView::ActionID::DumpCookies:
        set_icon("preferences-web-browser-cookies-symbolic");
        break;
    case WebView::ActionID::DumpLocalStorage:
        set_icon("drive-harddisk-symbolic");
        break;
    case WebView::ActionID::ShowLineBoxBorders:
        set_icon("view-grid-symbolic");
        break;
    case WebView::ActionID::CollectGarbage:
        set_icon("user-trash-symbolic");
        set_accel("<Ctrl><Shift>g");
        break;

    default:
        break;
    }
}

static void add_items_to_menu(GMenu& menu, ReadonlySpan<WebView::Menu::MenuItem> menu_items, Function<ByteString(WebView::Action&)> const& detailed_action_name_for_action)
{
    GObjectPtr current_section { g_menu_new() };
    bool section_has_items = false;

    auto flush_section = [&] {
        if (!section_has_items)
            return;
        g_menu_append_section(&menu, nullptr, G_MENU_MODEL(current_section.ptr()));
        current_section = GObjectPtr<GMenu> { g_menu_new() };
        section_has_items = false;
    };

    for (auto& menu_item : menu_items) {
        menu_item.visit(
            [&](NonnullRefPtr<WebView::Action> const& action) {
                if (!action->visible())
                    return;

                auto label = action->text().to_byte_string();
                auto detailed_action_name = detailed_action_name_for_action(*action);
                GObjectPtr gitem { g_menu_item_new(label.characters(), detailed_action_name.characters()) };
                initialize_native_control(*action, nullptr, G_MENU_ITEM(gitem.ptr()));
                g_menu_append_item(G_MENU(current_section.ptr()), G_MENU_ITEM(gitem.ptr()));
                section_has_items = true;
            },
            [&](NonnullRefPtr<WebView::Menu> const& submenu) {
                GObjectPtr submenu_model { create_application_menu(*submenu, detailed_action_name_for_action) };
                auto title = submenu->title().to_byte_string();
                GObjectPtr gitem { g_menu_item_new_submenu(title.characters(), G_MENU_MODEL(submenu_model.ptr())) };

                if (submenu->render_group_icon())
                    set_menu_item_icon_name(G_MENU_ITEM(gitem.ptr()), "folder-symbolic");

                g_menu_append_item(G_MENU(current_section.ptr()), G_MENU_ITEM(gitem.ptr()));
                section_has_items = true;
            },
            [&](WebView::Separator) {
                flush_section();
            });
    }

    flush_section();
}

class ContextMenu final {
public:
    ContextMenu(GtkWidget& parent, WebView::Menu& source)
        : m_popover(GTK_POPOVER(gtk_popover_menu_new_from_model(nullptr)))
    {
        gtk_widget_set_parent(GTK_WIDGET(m_popover), &parent);
        m_action_group = GObjectPtr<GSimpleActionGroup> { g_simple_action_group_new() };
        gtk_widget_insert_action_group(GTK_WIDGET(m_popover), "context", G_ACTION_GROUP(m_action_group.ptr()));

        size_t action_index = 0;
        source.for_each_action([&](WebView::Action& action) {
            auto action_name = ByteString::formatted("item-{}", action_index++);
            m_action_names.set(&action, action_name);
            add_action_to_map(G_ACTION_MAP(m_action_group.ptr()), action_name.characters(), action);
        });
    }

    ~ContextMenu()
    {
        gtk_widget_unparent(GTK_WIDGET(m_popover));
    }

    void popup(WebContentView& view, WebView::Menu& source, Gfx::IntPoint position)
    {
        GObjectPtr menu_model { create_application_menu(source, [&](WebView::Action& action) {
            return ByteString::formatted("context.{}", m_action_names.get(&action).value());
        }) };
        gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(m_popover), G_MENU_MODEL(menu_model.ptr()));

        auto device_pixel_ratio = view.device_pixel_ratio();
        GdkRectangle rect = {
            static_cast<int>(position.x() / device_pixel_ratio),
            static_cast<int>(position.y() / device_pixel_ratio),
            1, 1
        };
        gtk_popover_set_pointing_to(m_popover, &rect);
        gtk_popover_popup(m_popover);
    }

private:
    GtkPopover* m_popover { nullptr };
    GObjectPtr<GSimpleActionGroup> m_action_group;
    HashMap<WebView::Action const*, ByteString> m_action_names;
};

static GSimpleAction* create_application_action(char const* action_name, WebView::Action& action, bool observe_state)
{
    GSimpleAction* gaction = action.is_checkable()
        ? g_simple_action_new_stateful(action_name, nullptr, g_variant_new_boolean(action.checked()))
        : g_simple_action_new(action_name, nullptr);

    auto* weak_action = new WeakPtr<WebView::Action>(action.make_weak_ptr());
    g_signal_connect_data(gaction, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
            auto* weak = static_cast<WeakPtr<WebView::Action>*>(user_data);
            if (auto action = weak->strong_ref()) {
                if (action->is_checkable())
                    action->set_checked(!action->checked());
                action->activate();
            } }), weak_action, +[](gpointer data, GClosure*) { delete static_cast<WeakPtr<WebView::Action>*>(data); }, static_cast<GConnectFlags>(0));

    initialize_native_control(action, gaction, nullptr);
    if (observe_state)
        action.add_observer(make<ActionObserver>(gaction));
    return gaction;
}

void add_action_to_map(GActionMap* action_map, char const* action_name, WebView::Action& action, bool observe_state)
{
    GObjectPtr gaction { create_application_action(action_name, action, observe_state) };
    g_action_map_add_action(action_map, G_ACTION(gaction.ptr()));
}

GMenu* create_application_menu(WebView::Menu& menu, Function<ByteString(WebView::Action&)> const& detailed_action_name_for_action)
{
    auto* gmenu = g_menu_new();
    add_items_to_menu(*gmenu, menu.items(), detailed_action_name_for_action);
    return gmenu;
}

void create_context_menu(GtkWidget& parent, WebContentView& view, WebView::Menu& menu)
{
    auto context_menu = make<ContextMenu>(parent, menu);
    menu.on_activation = [context_menu = move(context_menu), &view, weak_menu = menu.make_weak_ptr()](Gfx::IntPoint position) {
        if (auto strong_menu = weak_menu.strong_ref())
            context_menu->popup(view, *strong_menu, position);
    };
}

void add_menu_actions_to_map(GActionMap* action_map, WebView::Menu& menu, Function<ByteString(WebView::Action&)> const& action_name_for_action)
{
    menu.for_each_action([&](WebView::Action& action) {
        auto action_name = action_name_for_action(action);
        add_action_to_map(action_map, action_name.characters(), action);
    });
}

void install_action_accelerators(GtkApplication* application, char const* detailed_action_name, WebView::Action const& action)
{
    switch (action.id()) {
    case WebView::ActionID::Reload: {
        static constexpr char const* accels[] = { "<Ctrl>r", "F5", nullptr };
        gtk_application_set_accels_for_action(application, detailed_action_name, accels);
        break;
    }
    case WebView::ActionID::ToggleDevTools: {
        static constexpr char const* accels[] = { "<Ctrl><Shift>i", "<Ctrl><Shift>c", "F12", nullptr };
        gtk_application_set_accels_for_action(application, detailed_action_name, accels);
        break;
    }
    case WebView::ActionID::ZoomIn: {
        static constexpr char const* accels[] = { "<Ctrl>equal", "<Ctrl>plus", nullptr };
        gtk_application_set_accels_for_action(application, detailed_action_name, accels);
        break;
    }
    case WebView::ActionID::NavigateBack:
    case WebView::ActionID::NavigateForward:
    case WebView::ActionID::ToggleBookmark:
    case WebView::ActionID::ToggleBookmarksBar:
    case WebView::ActionID::OpenProcessesPage:
    case WebView::ActionID::OpenSettingsPage:
    case WebView::ActionID::ViewSource:
    case WebView::ActionID::ZoomOut:
    case WebView::ActionID::ResetZoom:
    case WebView::ActionID::ResetZoomViaToolbar:
    case WebView::ActionID::CollectGarbage: {
        auto const* accel = primary_accelerator_for_action(action.id());
        if (!accel)
            return;
        char const* accels[] = { accel, nullptr };
        gtk_application_set_accels_for_action(application, detailed_action_name, accels);
        break;
    }
    default:
        break;
    }
}

void install_menu_action_accelerators(GtkApplication* application, char const* prefix, WebView::Menu& menu)
{
    menu.for_each_action([&](WebView::Action& action) {
        auto detailed_action_name = ByteString::formatted("{}-{}", prefix, static_cast<int>(action.id()));
        install_action_accelerators(application, detailed_action_name.characters(), action);
    });
}

void append_submenu_to_section_containing_action(GMenu* menu, char const* detailed_action_name, char const* submenu_label, GMenuModel* submenu_model)
{
    int n_items = g_menu_model_get_n_items(G_MENU_MODEL(menu));
    for (int i = 0; i < n_items; ++i) {
        GObjectPtr section { g_menu_model_get_item_link(G_MENU_MODEL(menu), i, G_MENU_LINK_SECTION) };
        if (!section.ptr())
            continue;

        int section_items = g_menu_model_get_n_items(G_MENU_MODEL(section.ptr()));
        for (int j = 0; j < section_items; ++j) {
            g_autofree char* action = nullptr;
            g_menu_model_get_item_attribute(G_MENU_MODEL(section.ptr()), j, G_MENU_ATTRIBUTE_ACTION, "s", &action);
            if (action && g_strcmp0(action, detailed_action_name) == 0) {
                g_menu_append_submenu(G_MENU(section.ptr()), submenu_label, submenu_model);
                return;
            }
        }
    }

    g_menu_append_submenu(menu, submenu_label, submenu_model);
}

}
