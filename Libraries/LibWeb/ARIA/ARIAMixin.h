/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/AttributeNames.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::ARIA {

#define ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_active_descendant_element, aria_active_descendant)

#define ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES                      \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_controls_elements, aria_controls)           \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_described_by_elements, aria_described_by)   \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_details_elements, aria_details)             \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_error_message_elements, aria_error_message) \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_flow_to_elements, aria_flow_to)             \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_labelled_by_elements, aria_labelled_by)     \
    __ENUMERATE_ARIA_ATTRIBUTE(aria_owns_elements, aria_owns)

class ARIAMixin {
public:
    virtual ~ARIAMixin();

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute) \
    virtual Optional<String> name() const = 0;      \
    virtual WebIDL::ExceptionOr<void> set_##name(Optional<String> const&) = 0;
    ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    // https://www.w3.org/TR/html-aria/#docconformance
    virtual Optional<Role> default_role() const { return {}; }

    virtual DOM::Element& to_element() = 0;
    virtual DOM::Element const& to_element() const = 0;

    Optional<Role> role_from_role_attribute_value() const;
    Optional<Role> role_or_default() const;

    // https://www.w3.org/TR/wai-aria-1.2/#tree_exclusion
    virtual bool exclude_from_accessibility_tree() const = 0;

    // https://www.w3.org/TR/wai-aria-1.2/#tree_inclusion
    virtual bool include_in_accessibility_tree() const = 0;

    bool has_global_aria_attribute() const;

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_idref
    Optional<String> parse_id_reference(Optional<String> const&) const;

    // https://www.w3.org/TR/wai-aria-1.2/#valuetype_idref_list
    Vector<String> parse_id_reference_list(Optional<String> const&) const;

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    GC::Ptr<DOM::Element> attribute() const;                         \
    void set_##attribute(GC::Ptr<DOM::Element> value);
    ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute)  \
    Optional<Vector<WeakPtr<DOM::Element>>> const& attribute() const; \
    void set_##attribute(Optional<Vector<WeakPtr<DOM::Element>>>);    \
                                                                      \
    GC::Ptr<JS::Array> cached_##attribute() const;                    \
    void set_cached_##attribute(GC::Ptr<JS::Array>);
    ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

protected:
    ARIAMixin();

    void visit_edges(GC::Cell::Visitor&);

    virtual bool id_reference_exists(String const&) const = 0;

private:
#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    WeakPtr<DOM::Element> m_##attribute;
    ENUMERATE_ARIA_ELEMENT_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

#define __ENUMERATE_ARIA_ATTRIBUTE(attribute, referencing_attribute) \
    Optional<Vector<WeakPtr<DOM::Element>>> m_##attribute;           \
    GC::Ptr<JS::Array> m_cached_##attribute;
    ENUMERATE_ARIA_ELEMENT_LIST_REFERENCING_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE
};

}
