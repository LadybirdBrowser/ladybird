/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/Roles.h>

namespace Web::ARIA {

Utf16String const& role_name(Role role)
{
    switch (role) {
#define __ENUMERATE_ARIA_ROLE(name, attribute)                                \
    case Role::name:                                                          \
        static Utf16String const& name = *new Utf16String(attribute##_utf16); \
        return name;
        ENUMERATE_ARIA_ROLES
#undef __ENUMERATE_ARIA_ROLE
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<Role> role_from_string(Utf16View input_role_name)
{
#define __ENUMERATE_ARIA_ROLE(name, attribute)                                          \
    if (input_role_name.equals_ignoring_ascii_case(role_name(Role::name).utf16_view())) \
        return Role::name;
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

Optional<AriaLive> implicit_aria_live_for_role(Role role)
{
    // ARIA 1.2 gives alert, log, and status a default aria-live in their Implicit Value for Role entries,
    // and marquee and timer none. Blink, WebKit, and Gecko all make those two off (GetImplicitAriaLive(),
    // defaultLiveRegionStatusForRole(), and the eOffLiveAttr rule of their role-map entries) — so an update
    // inside one isn't announced through an enclosing polite region — and so do we.
    switch (role) {
    case Role::alert:
        return AriaLive::Assertive;
    case Role::log:
    case Role::status:
        return AriaLive::Polite;
    case Role::marquee:
    case Role::timer:
        return AriaLive::Off;
    default:
        return {};
    }
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
