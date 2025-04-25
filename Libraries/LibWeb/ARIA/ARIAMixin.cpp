/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::ARIA {

ARIAMixin::ARIAMixin() = default;
ARIAMixin::~ARIAMixin() = default;

void ARIAMixin::visit_edges(GC::Cell::Visitor& visitor) {
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    visitor.visit(m_cached_##attribute);
    ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE
}

// https://www.w3.org/TR/wai-aria-1.2/#introroles
Optional<Role> ARIAMixin::role_from_role_attribute_value() const
{
    // 1. Use the rules of the host language to detect that an element has a role attribute and to identify the attribute value string for it.
    auto maybe_role_string = role();
    if (!maybe_role_string.has_value())
        return OptionalNone {};

    // 2. Separate the attribute value string for that attribute into a sequence of whitespace-free substrings by separating on whitespace.
    auto role_string = maybe_role_string.value();
    auto role_list = role_string.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);

    // 3. Compare the substrings to all the names of the non-abstract WAI-ARIA roles. Case-sensitivity of the comparison inherits from the case-sensitivity of the host language.
    for (auto const& role_name : role_list) {
        auto role = role_from_string(role_name);
        if (!role.has_value())
            continue;
        // NOTE: Per https://w3c.github.io/aria/#directory, "Authors are advised to treat directory as deprecated and to
        // use 'list'." Further, the "directory role == computedrole list" and "div w/directory role == computedrole
        // list" tests in https://wpt.fyi/results/wai-aria/role/synonym-roles.html expect "list", not "directory".
        if (role == Role::directory)
            return Role::list;
        // NOTE: The "image" role value is a synonym for the older "img" role value; however, the "synonym img role ==
        // computedrole image" test in https://wpt.fyi/results/wai-aria/role/synonym-roles.html expects "image", not "img".
        if (role == Role::img)
            return Role::image;
        // https://w3c.github.io/core-aam/#roleMappingComputedRole
        // When an element has a role but is not contained in the required context (for example, an orphaned listitem
        // without the required accessible parent of role list), User Agents MUST ignore the role token, and return the
        // computedrole as if the ignored role token had not been included.
        if (role == ARIA::Role::columnheader) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::row)
                    return ARIA::Role::columnheader;
            }
            continue;
        }
        if (role == ARIA::Role::gridcell) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::row)
                    return ARIA::Role::gridcell;
            }
            continue;
        }
        if (role == ARIA::Role::listitem) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::directory, ARIA::Role::list))
                    return ARIA::Role::listitem;
            }
            continue;
        }
        if (role == ARIA::Role::menuitem) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::menu, ARIA::Role::menubar))
                    return ARIA::Role::menuitem;
            }
            continue;
        }
        if (role == ARIA::Role::menuitemcheckbox) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::menu, ARIA::Role::menubar))
                    return ARIA::Role::menuitemcheckbox;
            }
            continue;
        }
        if (role == ARIA::Role::menuitemradio) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::menu, ARIA::Role::menubar))
                    return ARIA::Role::menuitemradio;
            }
            continue;
        }
        if (role == ARIA::Role::option) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::listbox)
                    return ARIA::Role::option;
            }
            continue;
        }
        if (role == ARIA::Role::row) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::table, ARIA::Role::grid, ARIA::Role::treegrid))
                    return ARIA::Role::row;
            }
            continue;
        }
        if (role == ARIA::Role::rowgroup) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::table, ARIA::Role::grid, ARIA::Role::treegrid))
                    return ARIA::Role::rowgroup;
            }
            continue;
        }
        if (role == ARIA::Role::rowheader) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::row)
                    return ARIA::Role::rowheader;
            }
            continue;
        }
        if (role == ARIA::Role::tab) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::tablist)
                    return ARIA::Role::tab;
            }
            continue;
        }
        if (role == ARIA::Role::treeitem) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::tree)
                    return ARIA::Role::treeitem;
            }
            continue;
        }
        // https://w3c.github.io/aria/#document-handling_author-errors_roles
        // Certain landmark roles require names from authors. In situations where an author has not specified names for
        // these landmarks, it is considered an authoring error. The user agent MUST treat such elements as if no role
        // had been provided. If a valid fallback role had been specified, or if the element had an implicit ARIA role,
        // then user agents would continue to expose that role, instead.
        if ((role == ARIA::Role::form || role == ARIA::Role::region)
            && to_element().accessible_name(to_element().document(), DOM::ShouldComputeRole::No).value().is_empty())
            continue;
        if (role == ARIA::Role::none || role == ARIA::Role::presentation) {
            // https://w3c.github.io/aria/#conflict_resolution_presentation_none
            // If an element is focusable, user agents MUST ignore the none/presentation
            // role and expose the element with its implicit role.
            if (to_element().is_focusable())
                continue;
            // If an element has global WAI-ARIA states or properties, user agents MUST
            // ignore the none/presentation role and instead expose the element's implicit role.
            if (has_global_aria_attribute())
                continue;
            // NOTE: Per https://w3c.github.io/aria/#presentation, "the working group introduced 'none' as the preferred
            // synonym to the presentation role"; further, https://wpt.fyi/results/wai-aria/role/synonym-roles.html has
            // a "synonym presentation role == computedrole none" test that expects "none", not "presentation".
            if (role == Role::presentation)
                return Role::none;
        }
        // 4. Use the first such substring in textual order that matches the name of a non-abstract WAI-ARIA role.
        if (!is_abstract_role(*role))
            return *role;
    }

    // https://www.w3.org/TR/wai-aria-1.2/#document-handling_author-errors_roles
    // If the role attribute contains no tokens matching the name of a non-abstract WAI-ARIA role, the user agent MUST treat the element as if no role had been provided.
    // https://www.w3.org/TR/wai-aria-1.2/#implicit_semantics
    return OptionalNone {};
}

Optional<Role> ARIAMixin::role_or_default() const
{
    if (auto role = role_from_role_attribute_value(); role.has_value())
        return role;
    return default_role();
}

// https://www.w3.org/TR/wai-aria-1.2/#global_states
bool ARIAMixin::has_global_aria_attribute() const
{
    return aria_atomic().has_value()
        || aria_braille_label().has_value()
        || aria_braille_role_description().has_value()
        || aria_busy().has_value()
        || aria_controls().has_value()
        || aria_current().has_value()
        || aria_described_by().has_value()
        || aria_description().has_value()
        || aria_details().has_value()
        || aria_disabled().has_value()
        || aria_drop_effect().has_value()
        || aria_error_message().has_value()
        || aria_flow_to().has_value()
        || aria_grabbed().has_value()
        || aria_has_popup().has_value()
        || aria_hidden().has_value()
        || aria_invalid().has_value()
        || aria_key_shortcuts().has_value()
        || aria_label().has_value()
        || aria_labelled_by().has_value()
        || aria_live().has_value()
        || aria_owns().has_value()
        || aria_relevant().has_value()
        || aria_role_description().has_value();
}

Optional<String> ARIAMixin::parse_id_reference(Optional<String> const& id_reference) const
{
    if (!id_reference.has_value())
        return {};

    if (id_reference_exists(id_reference.value()))
        return id_reference.value();

    return {};
}

Vector<String> ARIAMixin::parse_id_reference_list(Optional<String> const& id_list) const
{
    Vector<String> result;
    if (!id_list.has_value())
        return result;

    auto id_references = id_list->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
    for (auto const id_reference_view : id_references) {
        auto id_reference = MUST(String::from_utf8(id_reference_view));
        if (id_reference_exists(id_reference))
            result.append(id_reference);
    }
    return result;
}

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    GC::Ptr<DOM::Element> ARIAMixin::attribute() const               \
    {                                                                \
        return m_##attribute.ptr();                                  \
    }                                                                \
                                                                     \
    void ARIAMixin::set_##attribute(GC::Ptr<DOM::Element> value)     \
    {                                                                \
        m_##attribute = value.ptr();                                 \
    }
ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)               \
    Optional<Vector<WeakPtr<DOM::Element>>> const& ARIAMixin::attribute() const    \
    {                                                                              \
        return m_##attribute;                                                      \
    }                                                                              \
                                                                                   \
    void ARIAMixin::set_##attribute(Optional<Vector<WeakPtr<DOM::Element>>> value) \
    {                                                                              \
        m_##attribute = move(value);                                               \
    }                                                                              \
                                                                                   \
    GC::Ptr<JS::Array> ARIAMixin::cached_##attribute() const                       \
    {                                                                              \
        return m_cached_##attribute;                                               \
    }                                                                              \
                                                                                   \
    void ARIAMixin::set_cached_##attribute(GC::Ptr<JS::Array> value)               \
    {                                                                              \
        m_cached_##attribute = value;                                              \
    }
ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

}
