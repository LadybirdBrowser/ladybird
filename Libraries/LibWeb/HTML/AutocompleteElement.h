/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWebCommon/HTML/FocusTrigger.h>

namespace Web::HTML {

#define AUTOCOMPLETE_ELEMENT(ElementBaseClass, ElementClass)             \
private:                                                                 \
    virtual HTMLElement& autocomplete_element_to_html_element() override \
    {                                                                    \
        static_assert(IsBaseOf<HTMLElement, ElementClass>);              \
        static_assert(IsBaseOf<FormAssociatedElement, ElementClass>);    \
        return *this;                                                    \
    }

class WEB_API AutocompleteElement {
public:
    enum class AutofillMantle {
        Anchor,
        Expectation,
    };
    AutofillMantle get_autofill_mantle() const;

    Vector<Utf16String> autocomplete_tokens() const;
    Utf16String autocomplete() const;
    void set_autocomplete(Utf16View);

    // Each input element to which the autocomplete attribute applies [...] has
    // an autofill hint set, an autofill scope, an autofill field name,
    // a non-autofill credential type, and an IDL-exposed autofill value.
    struct AttributeDetails {
        Vector<Utf16FlyString> hint_set;
        Vector<Utf16String> scope;
        Utf16String field_name;
        Optional<Utf16FlyString> credential_type;
        Utf16String value;
    };
    AttributeDetails parse_autocomplete_attribute() const;

    // https://w3c.github.io/webauthn/#GetAssn-ConditionalMediation-Interact-FormControl
    // NB: The browser process holds the request with conditional mediation, if the document has one, and offers the
    //     credentials for it when the user interacts with a form control that has the "webauthn" token.
    void user_interacted_with_web_authentication_field();
    void web_authentication_field_received_focus(FocusTrigger);
    // NB: Input also says so, once, for a control that has the focus without the user giving it, such as with
    //     autofocus.
    void user_typed_in_web_authentication_field();
    void conditional_web_authentication_request_started();

    virtual HTMLElement& autocomplete_element_to_html_element() = 0;
    HTMLElement const& autocomplete_element_to_html_element() const { return const_cast<AutocompleteElement&>(*this).autocomplete_element_to_html_element(); }

protected:
    AutocompleteElement() = default;
    virtual ~AutocompleteElement() = default;

private:
    // Whether the user interacted with the control, as far as conditional mediation goes, since it got the focus.
    bool m_user_interacted_since_focus { false };
};

}
