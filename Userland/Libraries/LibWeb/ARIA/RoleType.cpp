/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/ARIA/AriaRoles.h>
#include <LibWeb/ARIA/RoleType.h>

namespace Web::ARIA {

RoleType::RoleType(AriaData const& data)
    : m_data(data)
{
}

// https://w3c.github.io/aria/#global_states
constexpr StateAndProperties supported_state_array[] = {
    StateAndProperties::AriaBusy,
    StateAndProperties::AriaCurrent,
    StateAndProperties::AriaDisabled,
    StateAndProperties::AriaGrabbed,
    StateAndProperties::AriaHidden,
    StateAndProperties::AriaInvalid
};
// https://w3c.github.io/aria/#global_states
constexpr StateAndProperties supported_properties_array[] = {
    StateAndProperties::AriaAtomic,
    StateAndProperties::AriaBrailleLabel,
    StateAndProperties::AriaBrailleRoleDescription,
    StateAndProperties::AriaControls,
    StateAndProperties::AriaDescribedBy,
    StateAndProperties::AriaDescription,
    StateAndProperties::AriaDetails,
    StateAndProperties::AriaDropEffect,
    StateAndProperties::AriaFlowTo,
    StateAndProperties::AriaHasPopup,
    StateAndProperties::AriaKeyShortcuts,
    StateAndProperties::AriaLabel,
    StateAndProperties::AriaLabelledBy,
    StateAndProperties::AriaLive,
    StateAndProperties::AriaOwns,
    StateAndProperties::AriaRelevant,
    StateAndProperties::AriaRoleDescription
};

HashTable<StateAndProperties> const& RoleType::supported_states() const
{
    static HashTable<StateAndProperties> states;
    if (states.is_empty())
        states.set_from(supported_state_array);
    return states;
}

HashTable<StateAndProperties> const& RoleType::supported_properties() const
{
    static HashTable<StateAndProperties> properties;
    if (properties.is_empty())
        properties.set_from(supported_properties_array);
    return properties;
}

HashTable<StateAndProperties> const& RoleType::required_states() const
{
    static HashTable<StateAndProperties> states;
    return states;
}

HashTable<StateAndProperties> const& RoleType::required_properties() const
{
    static HashTable<StateAndProperties> properties;
    return properties;
}

HashTable<StateAndProperties> const& RoleType::prohibited_properties() const
{
    static HashTable<StateAndProperties> properties;
    return properties;
}

HashTable<StateAndProperties> const& RoleType::prohibited_states() const
{
    static HashTable<StateAndProperties> states;
    return states;
}

HashTable<Role> const& RoleType::required_context_roles() const
{
    static HashTable<Role> roles;
    return roles;
}

HashTable<Role> const& RoleType::required_owned_elements() const
{
    static HashTable<Role> roles;
    return roles;
}

ErrorOr<void> RoleType::serialize_as_json(JsonObjectSerializer<StringBuilder>& object) const
{
    auto state_object = TRY(object.add_object("state"sv));
    for (auto const state : supported_states()) {
        auto value = TRY(ARIA::state_or_property_to_string_value(state, m_data, default_value_for_property_or_state(state)));
        TRY(state_object.add(ARIA::state_or_property_to_string(state), value));
    }
    TRY(state_object.finish());

    auto properties_object = TRY(object.add_object("properties"sv));
    for (auto const property : supported_properties()) {
        auto value = TRY(ARIA::state_or_property_to_string_value(property, m_data, default_value_for_property_or_state(property)));
        TRY(properties_object.add(ARIA::state_or_property_to_string(property), value));
    }
    TRY(properties_object.finish());

    auto required_states_object = TRY(object.add_object("required_state"sv));
    for (auto const state : required_states()) {
        auto value = TRY(ARIA::state_or_property_to_string_value(state, m_data, default_value_for_property_or_state(state)));
        TRY(required_states_object.add(ARIA::state_or_property_to_string(state), value));
    }
    TRY(required_states_object.finish());

    auto required_properties_object = TRY(object.add_object("required_properties"sv));
    for (auto const property : required_properties()) {
        auto value = TRY(ARIA::state_or_property_to_string_value(property, m_data, default_value_for_property_or_state(property)));
        TRY(required_properties_object.add(ARIA::state_or_property_to_string(property), value));
    }
    TRY(required_properties_object.finish());

    auto prohibited_states_object = TRY(object.add_object("prohibited_state"sv));
    for (auto const state : prohibited_states()) {
        auto value = TRY(ARIA::state_or_property_to_string_value(state, m_data, default_value_for_property_or_state(state)));
        TRY(prohibited_states_object.add(ARIA::state_or_property_to_string(state), value));
    }
    TRY(prohibited_states_object.finish());

    auto prohibited_properties_object = TRY(object.add_object("prohibited_properties"sv));
    for (auto const property : required_properties()) {
        auto value = TRY(ARIA::state_or_property_to_string_value(property, m_data, default_value_for_property_or_state(property)));
        TRY(prohibited_properties_object.add(ARIA::state_or_property_to_string(property), value));
    }
    TRY(prohibited_properties_object.finish());

    return {};
}

ErrorOr<NonnullOwnPtr<RoleType>> RoleType::build_role_object(Role role, bool focusable, AriaData const& data)
{
    if (is_abstract_role(role))
        return Error::from_string_literal("Cannot construct a role object for an abstract role.");

    switch (role) {
    case Role::alert:
        return adopt_nonnull_own_or_enomem(new (nothrow) Alert(data));
    case Role::alertdialog:
        return adopt_nonnull_own_or_enomem(static_cast<Alert*>((new (nothrow) AlertDialog(data))));
    case Role::application:
        return adopt_nonnull_own_or_enomem(new (nothrow) Application(data));
    case Role::article:
        return adopt_nonnull_own_or_enomem(new (nothrow) Article(data));
    case Role::banner:
        return adopt_nonnull_own_or_enomem(new (nothrow) Banner(data));
    case Role::blockquote:
        return adopt_nonnull_own_or_enomem(new (nothrow) BlockQuote(data));
    case Role::button:
        return adopt_nonnull_own_or_enomem(new (nothrow) Button(data));
    case Role::caption:
        return adopt_nonnull_own_or_enomem(new (nothrow) Caption(data));
    case Role::cell:
        return adopt_nonnull_own_or_enomem(new (nothrow) Cell(data));
    case Role::checkbox:
        return adopt_nonnull_own_or_enomem(new (nothrow) CheckBox(data));
    case Role::code:
        return adopt_nonnull_own_or_enomem(new (nothrow) Code(data));
    case Role::columnheader:
        return adopt_nonnull_own_or_enomem(static_cast<Cell*>(new (nothrow) ColumnHeader(data)));
    case Role::combobox:
        return adopt_nonnull_own_or_enomem(new (nothrow) ComboBox(data));
    case Role::complementary:
        return adopt_nonnull_own_or_enomem(new (nothrow) Complementary(data));
    case Role::composite:
        return adopt_nonnull_own_or_enomem(new (nothrow) Composite(data));
    case Role::contentinfo:
        return adopt_nonnull_own_or_enomem(new (nothrow) ContentInfo(data));
    case Role::definition:
        return adopt_nonnull_own_or_enomem(new (nothrow) Definition(data));
    case Role::deletion:
        return adopt_nonnull_own_or_enomem(new (nothrow) Deletion(data));
    case Role::dialog:
        return adopt_nonnull_own_or_enomem(new (nothrow) Dialog(data));
    case Role::directory:
        return adopt_nonnull_own_or_enomem(new (nothrow) Directory(data));
    case Role::document:
        return adopt_nonnull_own_or_enomem(new (nothrow) Document(data));
    case Role::emphasis:
        return adopt_nonnull_own_or_enomem(new (nothrow) Emphasis(data));
    case Role::feed:
        return adopt_nonnull_own_or_enomem(new (nothrow) Feed(data));
    case Role::figure:
        return adopt_nonnull_own_or_enomem(new (nothrow) Figure(data));
    case Role::form:
        return adopt_nonnull_own_or_enomem(new (nothrow) Form(data));
    case Role::generic:
        return adopt_nonnull_own_or_enomem(new (nothrow) Generic(data));
    case Role::grid:
        return adopt_nonnull_own_or_enomem(static_cast<Composite*>(new (nothrow) Grid(data)));
    case Role::gridcell:
        return adopt_nonnull_own_or_enomem(static_cast<Cell*>(new (nothrow) GridCell(data)));
    case Role::group:
        return adopt_nonnull_own_or_enomem(new (nothrow) Group(data));
    case Role::heading:
        return adopt_nonnull_own_or_enomem(new (nothrow) Heading(data));
    case Role::img:
        return adopt_nonnull_own_or_enomem(new (nothrow) Img(data));
    case Role::input:
        return adopt_nonnull_own_or_enomem(new (nothrow) Input(data));
    case Role::insertion:
        return adopt_nonnull_own_or_enomem(new (nothrow) Insertion(data));
    case Role::landmark:
        return adopt_nonnull_own_or_enomem(new (nothrow) Landmark(data));
    case Role::link:
        return adopt_nonnull_own_or_enomem(new (nothrow) Link(data));
    case Role::list:
        return adopt_nonnull_own_or_enomem(new (nothrow) List(data));
    case Role::listbox:
        return adopt_nonnull_own_or_enomem(static_cast<Composite*>(new (nothrow) ListBox(data)));
    case Role::listitem:
        return adopt_nonnull_own_or_enomem(new (nothrow) ListItem(data));
    case Role::log:
        return adopt_nonnull_own_or_enomem(new (nothrow) Log(data));
    case Role::main:
        return adopt_nonnull_own_or_enomem(new (nothrow) Main(data));
    case Role::marquee:
        return adopt_nonnull_own_or_enomem(new (nothrow) Marquee(data));
    case Role::math:
        return adopt_nonnull_own_or_enomem(new (nothrow) Math(data));
    case Role::meter:
        return adopt_nonnull_own_or_enomem(new (nothrow) Meter(data));
    case Role::menu:
        return adopt_nonnull_own_or_enomem(static_cast<Composite*>(new (nothrow) Menu(data)));
    case Role::menubar:
        return adopt_nonnull_own_or_enomem(static_cast<Composite*>(new (nothrow) MenuBar(data)));
    case Role::menuitem:
        return adopt_nonnull_own_or_enomem(new (nothrow) MenuItem(data));
    case Role::menuitemcheckbox:
        return adopt_nonnull_own_or_enomem(new (nothrow) MenuItemCheckBox(data));
    case Role::menuitemradio:
        return adopt_nonnull_own_or_enomem(new (nothrow) MenuItemRadio(data));
    case Role::navigation:
        return adopt_nonnull_own_or_enomem(new (nothrow) Navigation(data));
    case Role::none:
        return adopt_nonnull_own_or_enomem(new (nothrow) Presentation(data));
    case Role::note:
        return adopt_nonnull_own_or_enomem(new (nothrow) Note(data));
    case Role::option:
        return adopt_nonnull_own_or_enomem(new (nothrow) Option(data));
    case Role::paragraph:
        return adopt_nonnull_own_or_enomem(new (nothrow) Paragraph(data));
    case Role::presentation:
        return adopt_nonnull_own_or_enomem(new (nothrow) Presentation(data));
    case Role::progressbar:
        return adopt_nonnull_own_or_enomem(new (nothrow) Progressbar(data));
    case Role::radio:
        return adopt_nonnull_own_or_enomem(new (nothrow) Radio(data));
    case Role::radiogroup:
        return adopt_nonnull_own_or_enomem(static_cast<Composite*>(new (nothrow) RadioGroup(data)));
    case Role::region:
        return adopt_nonnull_own_or_enomem(new (nothrow) Region(data));
    case Role::row:
        return adopt_nonnull_own_or_enomem(static_cast<Group*>(new (nothrow) Row(data)));
    case Role::rowgroup:
        return adopt_nonnull_own_or_enomem(new (nothrow) RowGroup(data));
    case Role::rowheader:
        return adopt_nonnull_own_or_enomem(static_cast<Cell*>(new (nothrow) RowHeader(data)));
    case Role::scrollbar:
        return adopt_nonnull_own_or_enomem(static_cast<Range*>(new (nothrow) Scrollbar(data)));
    case Role::search:
        return adopt_nonnull_own_or_enomem(new (nothrow) Search(data));
    case Role::searchbox:
        return adopt_nonnull_own_or_enomem(new (nothrow) SearchBox(data));
    case Role::separator:
        if (focusable)
            return adopt_nonnull_own_or_enomem(new (nothrow) FocusableSeparator(data));
        else
            return adopt_nonnull_own_or_enomem(new (nothrow) NonFocusableSeparator(data));
    case Role::slider:
        return adopt_nonnull_own_or_enomem(static_cast<Input*>(new (nothrow) Slider(data)));
    case Role::spinbutton:
        return adopt_nonnull_own_or_enomem(static_cast<Composite*>(new (nothrow) SpinButton(data)));
    case Role::status:
        return adopt_nonnull_own_or_enomem(new (nothrow) Status(data));
    case Role::strong:
        return adopt_nonnull_own_or_enomem(new (nothrow) Strong(data));
    case Role::subscript:
        return adopt_nonnull_own_or_enomem(new (nothrow) Subscript(data));
    case Role::superscript:
        return adopt_nonnull_own_or_enomem(new (nothrow) Superscript(data));
    case Role::switch_:
        return adopt_nonnull_own_or_enomem(new (nothrow) Switch(data));
    case Role::tab:
        return adopt_nonnull_own_or_enomem(static_cast<SectionHead*>(new (nothrow) Tab(data)));
    case Role::table:
        return adopt_nonnull_own_or_enomem(new (nothrow) Table(data));
    case Role::tablist:
        return adopt_nonnull_own_or_enomem(new (nothrow) TabList(data));
    case Role::tabpanel:
        return adopt_nonnull_own_or_enomem(new (nothrow) TabPanel(data));
    case Role::term:
        return adopt_nonnull_own_or_enomem(new (nothrow) Term(data));
    case Role::textbox:
        return adopt_nonnull_own_or_enomem(new (nothrow) TextBox(data));
    case Role::time:
        return adopt_nonnull_own_or_enomem(new (nothrow) Time(data));
    case Role::timer:
        return adopt_nonnull_own_or_enomem(new (nothrow) Timer(data));
    case Role::toolbar:
        return adopt_nonnull_own_or_enomem(new (nothrow) Toolbar(data));
    case Role::tooltip:
        return adopt_nonnull_own_or_enomem(new (nothrow) Tooltip(data));
    case Role::tree:
        return adopt_nonnull_own_or_enomem(static_cast<Composite*>(new (nothrow) Tree(data)));
    case Role::treegrid:
        return adopt_nonnull_own_or_enomem(static_cast<Group*>(new (nothrow) TreeGrid(data)));
    case Role::treeitem:
        return adopt_nonnull_own_or_enomem(static_cast<ListItem*>(new (nothrow) TreeItem(data)));
    case Role::window:
        return adopt_nonnull_own_or_enomem(new (nothrow) Window(data));
    default:
        VERIFY_NOT_REACHED();
    }
}

}
