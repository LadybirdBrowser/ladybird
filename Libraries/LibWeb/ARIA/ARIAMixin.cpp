/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::ARIA {

// https://www.w3.org/TR/wai-aria-1.2/#introroles
Optional<Role> ARIAMixin::role_or_default() const
{
    // 1. Use the rules of the host language to detect that an element has a role attribute and to identify the attribute value string for it.
    auto maybe_role_string = role();
    if (!maybe_role_string.has_value())
        return default_role();

    // 2. Separate the attribute value string for that attribute into a sequence of whitespace-free substrings by separating on whitespace.
    auto role_string = maybe_role_string.value();
    auto role_list = role_string.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);

    // 3. Compare the substrings to all the names of the non-abstract WAI-ARIA roles. Case-sensitivity of the comparison inherits from the case-sensitivity of the host language.
    for (auto const& role_name : role_list) {
        // 4. Use the first such substring in textual order that matches the name of a non-abstract WAI-ARIA role.
        auto role = role_from_string(role_name);
        if (!role.has_value())
            continue;
        if (is_non_abstract_role(*role))
            return *role;
    }

    // https://www.w3.org/TR/wai-aria-1.2/#document-handling_author-errors_roles
    // If the role attribute contains no tokens matching the name of a non-abstract WAI-ARIA role, the user agent MUST treat the element as if no role had been provided.
    // https://www.w3.org/TR/wai-aria-1.2/#implicit_semantics
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

}
