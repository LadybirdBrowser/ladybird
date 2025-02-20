/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/FormAssociatedElement.h>

namespace Web::HTML {

#define AUTOCOMPLETE_ELEMENT(ElementBaseClass, ElementClass)             \
private:                                                                 \
    virtual HTMLElement& autocomplete_element_to_html_element() override \
    {                                                                    \
        static_assert(IsBaseOf<HTMLElement, ElementClass>);              \
        static_assert(IsBaseOf<FormAssociatedElement, ElementClass>);    \
        return *this;                                                    \
    }

class AutocompleteElement {
public:
    enum class AutofillMantle {
        Anchor,
        Expectation,
    };
    AutofillMantle get_autofill_mantle() const;

    Vector<String> autocomplete_tokens() const;
    String autocomplete() const;
    WebIDL::ExceptionOr<void> set_autocomplete(String const&);

    // Each input element to which the autocomplete attribute applies [...] has
    // an autofill hint set, an autofill scope, an autofill field name,
    // a non-autofill credential type, and an IDL-exposed autofill value.
    struct AttributeDetails {
        Vector<String> hint_set;
        Vector<String> scope;
        String field_name;
        Optional<String> credential_type;
        String value;
    };
    AttributeDetails parse_autocomplete_attribute() const;

    virtual HTMLElement& autocomplete_element_to_html_element() = 0;
    HTMLElement const& autocomplete_element_to_html_element() const { return const_cast<AutocompleteElement&>(*this).autocomplete_element_to_html_element(); }

protected:
    AutocompleteElement() = default;
    virtual ~AutocompleteElement() = default;
};

}
