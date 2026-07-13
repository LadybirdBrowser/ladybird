/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::ARIA {

ARIAMixin::ARIAMixin() = default;
ARIAMixin::~ARIAMixin() = default;

static void for_each_ascii_whitespace_separated_token(Utf16View input, Function<IterationDecision(Utf16View)> const& callback)
{
    size_t start = 0;
    for (size_t i = 0; i <= input.length_in_code_units(); ++i) {
        if (i != input.length_in_code_units() && !Infra::is_ascii_whitespace(input.code_unit_at(i)))
            continue;

        if (i > start && callback(input.substring_view(start, i - start)) == IterationDecision::Break)
            return;
        start = i + 1;
    }
}

void ARIAMixin::visit_edges(GC::Cell::Visitor& visitor)
{
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
    // 3. Compare the substrings to all the names of the non-abstract WAI-ARIA roles. Case-sensitivity of the comparison inherits from the case-sensitivity of the host language.
    Optional<Role> first_matching_role;
    for_each_ascii_whitespace_separated_token(maybe_role_string->utf16_view(), [&](auto role_name) {
        auto role = role_from_string(role_name);
        if (!role.has_value())
            return IterationDecision::Continue;
        // NOTE: Per https://w3c.github.io/aria/#directory, "Authors are advised to treat directory as deprecated and to
        // use 'list'." Further, the "directory role == computedrole list" and "div w/directory role == computedrole
        // list" tests in https://wpt.fyi/results/wai-aria/role/synonym-roles.html expect "list", not "directory".
        if (role == Role::directory) {
            first_matching_role = Role::list;
            return IterationDecision::Break;
        }
        // NOTE: The "image" role value is a synonym for the older "img" role value; however, the "synonym img role ==
        // computedrole image" test in https://wpt.fyi/results/wai-aria/role/synonym-roles.html expects "image", not "img".
        if (role == Role::img) {
            first_matching_role = Role::image;
            return IterationDecision::Break;
        }
        // https://w3c.github.io/core-aam/#roleMappingComputedRole
        // When an element has a role but is not contained in the required context (for example, an orphaned listitem
        // without the required accessible parent of role list), User Agents MUST ignore the role token, and return the
        // computedrole as if the ignored role token had not been included.
        if (role == ARIA::Role::columnheader) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::row) {
                    first_matching_role = ARIA::Role::columnheader;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::gridcell) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::row) {
                    first_matching_role = ARIA::Role::gridcell;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::listitem) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::directory, ARIA::Role::list)) {
                    first_matching_role = ARIA::Role::listitem;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::menuitem) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::menu, ARIA::Role::menubar)) {
                    first_matching_role = ARIA::Role::menuitem;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::menuitemcheckbox) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::menu, ARIA::Role::menubar)) {
                    first_matching_role = ARIA::Role::menuitemcheckbox;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::menuitemradio) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::menu, ARIA::Role::menubar)) {
                    first_matching_role = ARIA::Role::menuitemradio;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::option) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::listbox) {
                    first_matching_role = ARIA::Role::option;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::row) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::table, ARIA::Role::grid, ARIA::Role::treegrid)) {
                    first_matching_role = ARIA::Role::row;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::rowgroup) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (first_is_one_of(ancestor->role_or_default(), ARIA::Role::table, ARIA::Role::grid, ARIA::Role::treegrid)) {
                    first_matching_role = ARIA::Role::rowgroup;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::rowheader) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::row) {
                    first_matching_role = ARIA::Role::rowheader;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::tab) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::tablist) {
                    first_matching_role = ARIA::Role::tab;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        if (role == ARIA::Role::treeitem) {
            for (auto ancestor = to_element().parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->role_or_default() == ARIA::Role::tree) {
                    first_matching_role = ARIA::Role::treeitem;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        }
        // https://w3c.github.io/aria/#document-handling_author-errors_roles
        // Certain landmark roles require names from authors. In situations where an author has not specified names for
        // these landmarks, it is considered an authoring error. The user agent MUST treat such elements as if no role
        // had been provided. If a valid fallback role had been specified, or if the element had an implicit ARIA role,
        // then user agents would continue to expose that role, instead.
        if ((role == ARIA::Role::form || role == ARIA::Role::region)
            && to_element().accessible_name(to_element().document(), DOM::ShouldComputeRole::No).value().is_empty())
            return IterationDecision::Continue;
        if (role == ARIA::Role::none || role == ARIA::Role::presentation) {
            // https://w3c.github.io/aria/#conflict_resolution_presentation_none
            // If an element is focusable, user agents MUST ignore the none/presentation
            // role and expose the element with its implicit role.
            if (to_element().is_focusable())
                return IterationDecision::Continue;
            // If an element has global WAI-ARIA states or properties, user agents MUST
            // ignore the none/presentation role and instead expose the element's implicit role.
            if (has_global_aria_attribute())
                return IterationDecision::Continue;
            // NOTE: Per https://w3c.github.io/aria/#presentation, "the working group introduced 'none' as the preferred
            // synonym to the presentation role"; further, https://wpt.fyi/results/wai-aria/role/synonym-roles.html has
            // a "synonym presentation role == computedrole none" test that expects "none", not "presentation".
            if (role == Role::presentation) {
                first_matching_role = Role::none;
                return IterationDecision::Break;
            }
        }
        // 4. Use the first such substring in textual order that matches the name of a non-abstract WAI-ARIA role.
        if (!is_abstract_role(*role)) {
            first_matching_role = *role;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (first_matching_role.has_value())
        return first_matching_role;

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

Optional<Utf16String> ARIAMixin::parse_id_reference(Optional<Utf16String> const& id_reference) const
{
    if (!id_reference.has_value())
        return {};

    if (id_reference_exists(id_reference.value()))
        return id_reference.value();

    return {};
}

Vector<Utf16String> ARIAMixin::parse_id_reference_list(Optional<Utf16String> const& id_list) const
{
    Vector<Utf16String> result;
    if (!id_list.has_value())
        return result;

    for_each_ascii_whitespace_separated_token(id_list->utf16_view(), [&](auto id_reference_view) {
        if (id_reference_exists(id_reference_view))
            result.append(Utf16String::from_utf16(id_reference_view));
        return IterationDecision::Continue;
    });
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

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)                 \
    Optional<Vector<GC::Weak<DOM::Element>> const&> ARIAMixin::attribute() const     \
    {                                                                                \
        if (!m_##attribute)                                                          \
            return {};                                                               \
        return *m_##attribute;                                                       \
    }                                                                                \
                                                                                     \
    void ARIAMixin::set_##attribute(Optional<Vector<GC::Weak<DOM::Element>>> value)  \
    {                                                                                \
        if (!value.has_value()) {                                                    \
            m_##attribute.clear();                                                   \
            return;                                                                  \
        }                                                                            \
        m_##attribute = make<Vector<GC::Weak<DOM::Element>>>(value.release_value()); \
    }                                                                                \
                                                                                     \
    GC::Ptr<JS::Array> ARIAMixin::cached_##attribute() const                         \
    {                                                                                \
        return m_cached_##attribute;                                                 \
    }                                                                                \
                                                                                     \
    void ARIAMixin::set_cached_##attribute(GC::Ptr<JS::Array> value)                 \
    {                                                                                \
        m_cached_##attribute = value;                                                \
    }
ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

}
