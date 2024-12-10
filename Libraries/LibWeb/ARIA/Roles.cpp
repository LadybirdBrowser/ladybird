/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWeb/ARIA/Roles.h>

namespace Web::ARIA {

StringView role_name(Role role)
{
    // Note: Role::switch_ is mapped to "switch" (due to C++ keyword clash)
    switch (role) {
#define __ENUMERATE_ARIA_ROLE(name)                \
    case Role::name:                               \
        if constexpr (Role::name == Role::switch_) \
            return "switch"sv;                     \
        return #name##sv;
        ENUMERATE_ARIA_ROLES
#undef __ENUMERATE_ARIA_ROLE
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<Role> role_from_string(StringView role_name)
{
    // Note: "switch" is mapped to Role::switch_ (due to C++ keyword clash)
#define __ENUMERATE_ARIA_ROLE(name)                           \
    if constexpr (Role::name == Role::switch_) {              \
        if (role_name.equals_ignoring_ascii_case("switch"sv)) \
            return Role::switch_;                             \
    } else {                                                  \
        if (role_name.equals_ignoring_ascii_case(#name##sv))  \
            return Role::name;                                \
    }
    ENUMERATE_ARIA_ROLES
#undef __ENUMERATE_ARIA_ROLE
    return {};
}

// https://www.w3.org/TR/wai-aria-1.2/#abstract_roles
bool is_abstract_role(Role role)
{
    return first_is_one_of(role,
        Role::command,
        Role::composite,
        Role::input,
        Role::landmark,
        Role::range,
        Role::roletype,
        Role::section,
        Role::sectionhead,
        Role::select,
        Role::structure,
        Role::widget,
        Role::window);
}

// https://www.w3.org/TR/wai-aria-1.2/#widget_roles
bool is_widget_role(Role role)
{
    return first_is_one_of(role,
        Role::button,
        Role::checkbox,
        Role::gridcell,
        Role::link,
        Role::menuitem,
        Role::menuitemcheckbox,
        Role::menuitemradio,
        Role::option,
        Role::progressbar,
        Role::radio,
        Role::scrollbar,
        Role::searchbox,
        Role::separator, // TODO: Only when focusable
        Role::slider,
        Role::spinbutton,
        Role::switch_,
        Role::tab,
        Role::tabpanel,
        Role::textbox,
        Role::treeitem,
        Role::combobox,
        Role::grid,
        Role::listbox,
        Role::menu,
        Role::menubar,
        Role::radiogroup,
        Role::tablist,
        Role::tree,
        Role::treegrid);
}

// https://www.w3.org/TR/wai-aria-1.2/#document_structure_roles
bool is_document_structure_role(Role role)
{
    return first_is_one_of(role,
        Role::application,
        Role::article,
        Role::blockquote,
        Role::caption,
        Role::cell,
        Role::columnheader,
        Role::definition,
        Role::deletion,
        Role::directory,
        Role::document,
        Role::emphasis,
        Role::feed,
        Role::figure,
        Role::generic,
        Role::group,
        Role::heading,
        Role::img,
        Role::insertion,
        Role::list,
        Role::listitem,
        Role::math,
        Role::meter,
        Role::none,
        Role::note,
        Role::paragraph,
        Role::presentation,
        Role::row,
        Role::rowgroup,
        Role::rowheader,
        Role::separator, // TODO: Only when not focusable
        Role::strong,
        Role::subscript,
        Role::table,
        Role::term,
        Role::time,
        Role::toolbar,
        Role::tooltip);
}

// https://www.w3.org/TR/wai-aria-1.2/#landmark_roles
bool is_landmark_role(Role role)
{
    return first_is_one_of(role,
        Role::banner,
        Role::complementary,
        Role::contentinfo,
        Role::form,
        Role::main,
        Role::navigation,
        Role::region,
        Role::search);
}

// https://www.w3.org/TR/wai-aria-1.2/#live_region_roles
bool is_live_region_role(Role role)
{
    return first_is_one_of(role,
        Role::alert,
        Role::log,
        Role::marquee,
        Role::status,
        Role::timer);
}

// https://www.w3.org/TR/wai-aria-1.2/#window_roles
bool is_windows_role(Role role)
{
    return first_is_one_of(role,
        Role::alertdialog,
        Role::dialog);
}

bool is_non_abstract_role(Role role)
{
    return is_widget_role(role)
        || is_document_structure_role(role)
        || is_landmark_role(role)
        || is_live_region_role(role)
        || is_windows_role(role);
}

// https://www.w3.org/TR/wai-aria-1.2/#namefromcontent
bool allows_name_from_content(Role role)
{
    return first_is_one_of(role,
        Role::button,
        Role::cell,
        Role::checkbox,
        Role::columnheader,
        Role::gridcell,
        Role::heading,
        Role::link,
        Role::menuitem,
        Role::menuitemcheckbox,
        Role::menuitemradio,
        Role::option,
        Role::radio,
        Role::row,
        Role::rowheader,
        Role::sectionhead,
        Role::switch_,
        Role::tab,
        Role::tooltip,
        Role::treeitem);
}

}
