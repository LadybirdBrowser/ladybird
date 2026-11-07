/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Hub/ActionMirror.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <LibWebView/ViewImplementation.h>

namespace Hub {

ActionMirror& ActionMirror::the()
{
    // NB: Actions can go away while the process exits, and they tell this object when they do.
    static auto& s_the = *new ActionMirror;
    return s_the;
}

struct ActionObserver final : public WebView::Action::Observer {
    ActionObserver(u64 handle, WebView::Action const& action)
        : m_handle(handle)
        , m_action(&action)
    {
    }

    // NB: An action owns its observers, so this runs as the action goes away.
    virtual ~ActionObserver() override { ActionMirror::the().retire(m_handle, m_action); }

    virtual void on_text_changed(WebView::Action& action) override { did_change(action); }
    virtual void on_tooltip_changed(WebView::Action& action) override { did_change(action); }
    virtual void on_enabled_state_changed(WebView::Action& action) override { did_change(action); }
    virtual void on_visible_state_changed(WebView::Action& action) override { did_change(action); }
    virtual void on_engaged_state_changed(WebView::Action& action) override { did_change(action); }
    virtual void on_checked_state_changed(WebView::Action& action) override { did_change(action); }

    static void did_change(WebView::Action& action)
    {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_action_changed(ActionMirror::the().state_of(action));
    }

    u64 m_handle { 0 };
    WebView::Action const* m_action { nullptr };
};

u64 ActionMirror::handle_of(WebView::Action& action)
{
    if (auto handle = m_handles.get(&action); handle.has_value())
        return *handle;

    auto handle = m_next_handle++;
    m_handles.set(&action, handle);
    m_actions.set(handle, action.make_weak_ptr<WebView::Action>());
    action.add_observer(make<ActionObserver>(handle, action));
    return handle;
}

void ActionMirror::retire(u64 handle, WebView::Action const* action)
{
    m_actions.remove(handle);
    m_handles.remove(action);
    if (auto* ui = ConnectionFromUI::the())
        ui->async_action_retired(handle);
}

template<typename MenuOrAction>
static void copy_properties(MenuOrAction const& menu_or_action, Vector<String>& names, Vector<String>& values)
{
    for (auto const& [name, value] : menu_or_action.properties()) {
        names.append(MUST(String::from_utf8(name)));
        values.append(value);
    }
}

ActionState ActionMirror::state_of(WebView::Action& action)
{
    ActionState state;
    state.handle = handle_of(action);
    state.id = action.id();
    state.text = MUST(String::from_utf8(action.text()));
    if (action.has_tooltip())
        state.tooltip = MUST(String::from_utf8(action.tooltip()));
    state.enabled = action.enabled();
    state.visible = action.visible();
    state.engaged = action.engaged();
    if (action.is_checkable())
        state.checked = action.checked();
    if (auto icon = action.png_icon(); icon.has_value())
        state.png_icon = *icon;
    copy_properties(action, state.property_names, state.property_values);
    return state;
}

static MenuEntry menu_entry(MenuEntryKind kind)
{
    MenuEntry entry;
    entry.kind = kind;
    return entry;
}

Vector<MenuEntry> ActionMirror::entries_of(WebView::Menu& menu)
{
    Vector<MenuEntry> entries;
    auto add_menu = [&](auto& add_menu, WebView::Menu& menu) -> void {
        auto entry = menu_entry(MenuEntryKind::Submenu);
        entry.title = MUST(String::from_utf8(menu.title()));
        entry.visible = menu.visible();
        entry.is_group = menu.is_group();
        entry.render_group_icon = menu.render_group_icon();
        copy_properties(menu, entry.property_names, entry.property_values);
        entries.append(move(entry));

        for (auto& item : menu.items()) {
            item.visit(
                [&](NonnullRefPtr<WebView::Action>& action) {
                    auto action_entry = menu_entry(MenuEntryKind::Action);
                    action_entry.action = state_of(*action);
                    entries.append(move(action_entry));
                },
                [&](NonnullRefPtr<WebView::Menu>& submenu) {
                    add_menu(add_menu, *submenu);
                },
                [&](WebView::Separator) {
                    entries.append(menu_entry(MenuEntryKind::Separator));
                });
        }
        entries.append(menu_entry(MenuEntryKind::EndOfSubmenu));
    };
    add_menu(add_menu, menu);
    return entries;
}

ApplicationActions ActionMirror::application_actions()
{
    auto& application = Application::the();
    ApplicationActions actions;
    actions.reload_action = state_of(application.reload_action());
    actions.reload_bypassing_cache_action = state_of(application.reload_bypassing_cache_action());
    actions.copy_selection_action = state_of(application.copy_selection_action());
    actions.undo_action = state_of(application.undo_action());
    actions.redo_action = state_of(application.redo_action());
    actions.cut_selection_action = state_of(application.cut_selection_action());
    actions.paste_action = state_of(application.paste_action());
    actions.select_all_action = state_of(application.select_all_action());
    actions.open_about_page_action = state_of(application.open_about_page_action());
    actions.open_downloads_page_action = state_of(application.open_downloads_page_action());
    actions.open_settings_page_action = state_of(application.open_settings_page_action());
    actions.zoom_in_action = state_of(application.zoom_in_action());
    actions.zoom_out_action = state_of(application.zoom_out_action());
    actions.reset_zoom_action = state_of(application.reset_zoom_action());
    actions.force_dark_action = state_of(application.force_dark_action());
    actions.toggle_vertical_tabs_expanded_action = state_of(application.toggle_vertical_tabs_expanded_action());
    actions.toggle_menu_bar_action = state_of(application.toggle_menu_bar_action());
    actions.toggle_bookmark_bar_action = state_of(application.toggle_bookmark_bar_action());
    actions.view_source_action = state_of(application.view_source_action());
    actions.report_website_issue_action = state_of(application.report_website_issue_action());
    actions.zoom_menu = entries_of(application.zoom_menu());
    actions.color_scheme_menu = entries_of(application.color_scheme_menu());
    actions.contrast_menu = entries_of(application.contrast_menu());
    actions.motion_menu = entries_of(application.motion_menu());
    actions.bookmarks_menu = entries_of(application.bookmarks_menu());
    actions.show_bookmarks_bar_menu = entries_of(application.show_bookmarks_bar_menu());
    actions.history_menu = entries_of(application.history_menu());
    actions.inspect_menu = entries_of(application.inspect_menu());
    actions.debug_menu = entries_of(application.debug_menu());
    return actions;
}

ViewActions ActionMirror::view_actions(WebView::ViewImplementation& view)
{
    ViewActions actions;
    actions.navigate_back_action = state_of(view.navigate_back_action());
    actions.navigate_forward_action = state_of(view.navigate_forward_action());
    actions.toggle_bookmark_action = state_of(view.toggle_bookmark_action());
    actions.reset_zoom_action = state_of(view.reset_zoom_action());
    actions.bookmarks_bar_context_menu = entries_of(view.bookmarks_bar_context_menu());
    actions.bookmark_context_menu = entries_of(view.bookmark_context_menu());
    actions.bookmark_folder_context_menu = entries_of(view.bookmark_folder_context_menu());
    return actions;
}

void ActionMirror::activate(u64 handle, Optional<bool> checked)
{
    auto* action = m_actions.get(handle).value_or(nullptr);
    if (!action)
        return;
    // NB: The UI has flipped its copy already, and what an action does often depends on its state.
    if (checked.has_value())
        action->set_checked(*checked);
    action->activate();
}

}
