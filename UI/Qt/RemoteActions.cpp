/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/RemoteActions.h>

namespace Ladybird {

RemoteActions& RemoteActions::the()
{
    static RemoteActions s_the;
    return s_the;
}

// NB: Menus and actions hold their property names as views of string literals, which these stand in for.
static StringView interned_property_name(String const& name)
{
    static HashTable<ByteString> s_names;
    auto byte_string = name.to_byte_string();
    s_names.set(byte_string);
    return s_names.find(byte_string)->view();
}

template<typename MenuOrAction>
static void apply_properties(MenuOrAction& menu_or_action, ReadonlySpan<String> names, ReadonlySpan<String> values)
{
    for (size_t i = 0; i < names.size(); ++i)
        menu_or_action.add_property(interned_property_name(names[i]), values[i]);
}

void RemoteActions::apply_state(WebView::Action& action, Hub::ActionState const& state)
{
    if (action.text() != state.text)
        action.set_text(state.text);
    if (state.tooltip.has_value() && (!action.has_tooltip() || action.tooltip() != *state.tooltip))
        action.set_tooltip(*state.tooltip);
    action.set_png_icon(state.png_icon);
    if (action.enabled() != state.enabled)
        action.set_enabled(state.enabled);
    if (action.visible() != state.visible)
        action.set_visible(state.visible);
    if (action.engaged() != state.engaged)
        action.set_engaged(state.engaged);
    if (state.checked.has_value() && action.is_checkable() && action.checked() != *state.checked)
        action.set_checked(*state.checked);
    apply_properties(action, state.property_names, state.property_values);
}

NonnullRefPtr<WebView::Action> RemoteActions::action_for(Hub::ActionState const& state)
{
    if (auto* action = m_actions.get(state.handle).value_or(nullptr)) {
        apply_state(*action, state);
        return *action;
    }

    auto activate = [this, handle = state.handle] {
        Optional<bool> checked;
        if (auto* action = m_actions.get(handle).value_or(nullptr); action && action->is_checkable())
            checked = action->checked();
        HubConnection::the().async_activate_action(handle, checked);
    };
    auto action = state.checked.has_value()
        ? WebView::Action::create_checkable(state.text, state.id, move(activate))
        : WebView::Action::create(state.text, state.id, move(activate));
    apply_state(*action, state);
    m_actions.set(state.handle, action->make_weak_ptr<WebView::Action>());
    return action;
}

void RemoteActions::update(Hub::ActionState const& state)
{
    if (auto* action = m_actions.get(state.handle).value_or(nullptr))
        apply_state(*action, state);
}

void RemoteActions::retire(u64 handle)
{
    m_actions.remove(handle);
}

NonnullRefPtr<WebView::Menu> RemoteActions::menu_for(ReadonlySpan<Hub::MenuEntry> entries)
{
    Vector<NonnullRefPtr<WebView::Menu>> open_menus;
    RefPtr<WebView::Menu> outermost_menu;

    for (auto const& entry : entries) {
        switch (entry.kind) {
        case Hub::MenuEntryKind::Submenu: {
            auto menu = entry.is_group ? WebView::Menu::create_group(entry.title) : WebView::Menu::create(entry.title);
            menu->set_render_group_icon(entry.render_group_icon);
            menu->set_visible(entry.visible);
            apply_properties(*menu, entry.property_names, entry.property_values);
            if (!open_menus.is_empty())
                open_menus.last()->add_submenu(menu);
            else
                outermost_menu = menu;
            open_menus.append(menu);
            break;
        }
        case Hub::MenuEntryKind::EndOfSubmenu:
            (void)open_menus.take_last();
            break;
        case Hub::MenuEntryKind::Action:
            open_menus.last()->add_action(action_for(*entry.action));
            break;
        case Hub::MenuEntryKind::Separator:
            open_menus.last()->add_separator();
            break;
        }
    }
    return outermost_menu.release_nonnull();
}

void RemoteActions::set_application_actions(Hub::ApplicationActions const& actions)
{
    m_reload_action = action_for(actions.reload_action);
    m_reload_bypassing_cache_action = action_for(actions.reload_bypassing_cache_action);
    m_copy_selection_action = action_for(actions.copy_selection_action);
    m_undo_action = action_for(actions.undo_action);
    m_redo_action = action_for(actions.redo_action);
    m_cut_selection_action = action_for(actions.cut_selection_action);
    m_paste_action = action_for(actions.paste_action);
    m_select_all_action = action_for(actions.select_all_action);
    m_open_about_page_action = action_for(actions.open_about_page_action);
    m_open_downloads_page_action = action_for(actions.open_downloads_page_action);
    m_open_settings_page_action = action_for(actions.open_settings_page_action);
    m_zoom_in_action = action_for(actions.zoom_in_action);
    m_zoom_out_action = action_for(actions.zoom_out_action);
    m_reset_zoom_action = action_for(actions.reset_zoom_action);
    m_force_dark_action = action_for(actions.force_dark_action);
    m_toggle_vertical_tabs_expanded_action = action_for(actions.toggle_vertical_tabs_expanded_action);
    m_toggle_menu_bar_action = action_for(actions.toggle_menu_bar_action);
    m_view_source_action = action_for(actions.view_source_action);
    m_report_website_issue_action = action_for(actions.report_website_issue_action);
    m_zoom_menu = menu_for(actions.zoom_menu);
    m_color_scheme_menu = menu_for(actions.color_scheme_menu);
    m_contrast_menu = menu_for(actions.contrast_menu);
    m_motion_menu = menu_for(actions.motion_menu);
    m_history_menu = menu_for(actions.history_menu);
    m_inspect_menu = menu_for(actions.inspect_menu);
    m_debug_menu = menu_for(actions.debug_menu);
}

}
