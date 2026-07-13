/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/AutocompleteElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

namespace AutocompleteToken {

#define __ENUMERATE_AUTOCOMPLETE_TOKEN(name, value) Utf16FlyString const& name = *new Utf16FlyString(value##_utf16_fly_string);
__ENUMERATE_AUTOCOMPLETE_TOKEN(billing, "billing")
__ENUMERATE_AUTOCOMPLETE_TOKEN(fax, "fax")
__ENUMERATE_AUTOCOMPLETE_TOKEN(home, "home")
__ENUMERATE_AUTOCOMPLETE_TOKEN(mobile, "mobile")
__ENUMERATE_AUTOCOMPLETE_TOKEN(pager, "pager")
__ENUMERATE_AUTOCOMPLETE_TOKEN(shipping, "shipping")
__ENUMERATE_AUTOCOMPLETE_TOKEN(webauthn, "webauthn")
__ENUMERATE_AUTOCOMPLETE_TOKEN(work, "work")
#undef __ENUMERATE_AUTOCOMPLETE_TOKEN

}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#autofill-expectation-mantle
// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#autofill-anchor-mantle
AutocompleteElement::AutofillMantle AutocompleteElement::get_autofill_mantle() const
{
    auto const& element = autocomplete_element_to_html_element();

    // On an input element whose type attribute is in the Hidden state, the autocomplete attribute wears the autofill anchor mantle.
    if (is<HTMLInputElement>(element)) {
        auto const& input_element = as<HTMLInputElement>(element);
        if (input_element.type_state() == HTMLInputElement::TypeAttributeState::Hidden)
            return AutofillMantle::Anchor;
    }

    // In all other cases, it wears the autofill expectation mantle.
    return AutofillMantle::Expectation;
}

static Vector<Utf16View> autocomplete_tokens(Utf16View autocomplete_value)
{
    Vector<Utf16View> autocomplete_tokens;
    for (size_t start = 0, i = 0; i <= autocomplete_value.length_in_code_units(); ++i) {
        if (i != autocomplete_value.length_in_code_units() && !Infra::is_ascii_whitespace(autocomplete_value.code_unit_at(i)))
            continue;

        if (i > start)
            autocomplete_tokens.append(autocomplete_value.substring_view(start, i - start));
        start = i + 1;
    }
    return autocomplete_tokens;
}

Vector<Utf16String> AutocompleteElement::autocomplete_tokens() const
{
    auto autocomplete_value = autocomplete_element_to_html_element().attribute(AttributeNames::autocomplete);
    auto autocomplete_value_view = autocomplete_value.has_value() ? autocomplete_value->utf16_view() : u""sv;

    Vector<Utf16String> tokens;
    for (auto token : HTML::autocomplete_tokens(autocomplete_value_view))
        tokens.append(Utf16String::from_utf16(token));
    return tokens;
}

Utf16String AutocompleteElement::autocomplete() const
{
    // The autocomplete IDL attribute, on getting, must return the element's IDL-exposed autofill value.
    auto details = parse_autocomplete_attribute();
    return details.value;
}

void AutocompleteElement::set_autocomplete(Utf16View value)
{
    // The autocomplete IDL attribute [...] on setting, must reflect the content attribute of the same name.
    autocomplete_element_to_html_element().set_attribute_value(AttributeNames::autocomplete, value);
}

enum class Category {
    Off,
    Automatic,
    Normal,
    Contact,
    Credential,
};

struct CategoryAndMaximumTokens {
    Optional<Category> category;
    Optional<size_t> maximum_tokens;
};

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#determine-a-field's-category
static CategoryAndMaximumTokens determine_a_field_category(Utf16View const& field)
{
#define CASE_CATEGORY(token, maximum_number_of_tokens, category) \
    if (field.equals_ignoring_ascii_case(token))                 \
        return CategoryAndMaximumTokens { Category::category, maximum_number_of_tokens };

    // 1. If the field is not an ASCII case-insensitive match for one of the tokens given
    //    in the first column of the following table, return the pair (null, null).
    // 2. Otherwise, let maximum tokens and category be the values of the cells in the second
    //    and third columns of that row respectively.
    // 3. Return the pair (category, maximum tokens).
    CASE_CATEGORY(u"off"sv, 1, Off);
    CASE_CATEGORY(u"on"sv, 1, Automatic);
    CASE_CATEGORY(u"name"sv, 3, Normal);
    CASE_CATEGORY(u"honorific-prefix"sv, 3, Normal);
    CASE_CATEGORY(u"given-name"sv, 3, Normal);
    CASE_CATEGORY(u"additional-name"sv, 3, Normal);
    CASE_CATEGORY(u"family-name"sv, 3, Normal);
    CASE_CATEGORY(u"honorific-suffix"sv, 3, Normal);
    CASE_CATEGORY(u"nickname"sv, 3, Normal);
    CASE_CATEGORY(u"organization-title"sv, 3, Normal);
    CASE_CATEGORY(u"username"sv, 3, Normal);
    CASE_CATEGORY(u"new-password"sv, 3, Normal);
    CASE_CATEGORY(u"current-password"sv, 3, Normal);
    CASE_CATEGORY(u"one-time-code"sv, 3, Normal);
    CASE_CATEGORY(u"organization"sv, 3, Normal);
    CASE_CATEGORY(u"street-address"sv, 3, Normal);
    CASE_CATEGORY(u"address-line1"sv, 3, Normal);
    CASE_CATEGORY(u"address-line2"sv, 3, Normal);
    CASE_CATEGORY(u"address-line3"sv, 3, Normal);
    CASE_CATEGORY(u"address-level4"sv, 3, Normal);
    CASE_CATEGORY(u"address-level3"sv, 3, Normal);
    CASE_CATEGORY(u"address-level2"sv, 3, Normal);
    CASE_CATEGORY(u"address-level1"sv, 3, Normal);
    CASE_CATEGORY(u"country"sv, 3, Normal);
    CASE_CATEGORY(u"country-name"sv, 3, Normal);
    CASE_CATEGORY(u"postal-code"sv, 3, Normal);
    CASE_CATEGORY(u"cc-name"sv, 3, Normal);
    CASE_CATEGORY(u"cc-given-name"sv, 3, Normal);
    CASE_CATEGORY(u"cc-additional-name"sv, 3, Normal);
    CASE_CATEGORY(u"cc-family-name"sv, 3, Normal);
    CASE_CATEGORY(u"cc-number"sv, 3, Normal);
    CASE_CATEGORY(u"cc-exp"sv, 3, Normal);
    CASE_CATEGORY(u"cc-exp-month"sv, 3, Normal);
    CASE_CATEGORY(u"cc-exp-year"sv, 3, Normal);
    CASE_CATEGORY(u"cc-csc"sv, 3, Normal);
    CASE_CATEGORY(u"cc-type"sv, 3, Normal);
    CASE_CATEGORY(u"transaction-currency"sv, 3, Normal);
    CASE_CATEGORY(u"transaction-amount"sv, 3, Normal);
    CASE_CATEGORY(u"language"sv, 3, Normal);
    CASE_CATEGORY(u"bday"sv, 3, Normal);
    CASE_CATEGORY(u"bday-day"sv, 3, Normal);
    CASE_CATEGORY(u"bday-month"sv, 3, Normal);
    CASE_CATEGORY(u"bday-year"sv, 3, Normal);
    CASE_CATEGORY(u"sex"sv, 3, Normal);
    CASE_CATEGORY(u"url"sv, 3, Normal);
    CASE_CATEGORY(u"photo"sv, 3, Normal);
    CASE_CATEGORY(u"tel"sv, 4, Contact);
    CASE_CATEGORY(u"tel-country-code"sv, 4, Contact);
    CASE_CATEGORY(u"tel-national"sv, 4, Contact);
    CASE_CATEGORY(u"tel-area-code"sv, 4, Contact);
    CASE_CATEGORY(u"tel-local"sv, 4, Contact);
    CASE_CATEGORY(u"tel-local-prefix"sv, 4, Contact);
    CASE_CATEGORY(u"tel-local-suffix"sv, 4, Contact);
    CASE_CATEGORY(u"tel-extension"sv, 4, Contact);
    CASE_CATEGORY(u"email"sv, 4, Contact);
    CASE_CATEGORY(u"impp"sv, 4, Contact);
    CASE_CATEGORY(u"webauthn"sv, 5, Credential);

#undef CASE_CATEGORY

    return CategoryAndMaximumTokens {};
}

static Optional<Utf16FlyString> contact_token_canonicalized(Utf16View token)
{
    using namespace AutocompleteToken;

    if (token.equals_ignoring_ascii_case(u"home"sv))
        return home;
    if (token.equals_ignoring_ascii_case(u"work"sv))
        return work;
    if (token.equals_ignoring_ascii_case(u"mobile"sv))
        return mobile;
    if (token.equals_ignoring_ascii_case(u"fax"sv))
        return fax;
    if (token.equals_ignoring_ascii_case(u"pager"sv))
        return pager;
    return {};
}

static Optional<Utf16FlyString> mode_token_canonicalized(Utf16View token)
{
    using namespace AutocompleteToken;

    if (token.equals_ignoring_ascii_case(u"shipping"sv))
        return shipping;
    if (token.equals_ignoring_ascii_case(u"billing"sv))
        return billing;
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#autofill-processing-model
AutocompleteElement::AttributeDetails AutocompleteElement::parse_autocomplete_attribute() const
{
    AttributeDetails attr_details {};

    auto step_default = [&] {
        // 32. Default: Set the element's IDL-exposed autofill value to the empty string, and its autofill hint set and autofill scope to empty.
        attr_details.value = {};
        attr_details.hint_set = {};
        attr_details.scope = {};

        // 33. If the element's autocomplete attribute is wearing the autofill anchor mantle,
        //     then set the element's autofill field name to the empty string and return.
        if (get_autofill_mantle() == AutofillMantle::Anchor) {
            attr_details.field_name = {};
            return attr_details;
        }

        // 34. Let form be the element's form owner, if any, or null otherwise.
        auto const* form = as<FormAssociatedElement const>(autocomplete_element_to_html_element()).form();

        // 35. If form is not null and form's autocomplete attribute is in the off state, then set the element's autofill field name to "off".
        auto form_autocomplete_attribute = form ? form->attribute(AttributeNames::autocomplete) : Optional<Utf16String> {};
        auto form_autocomplete_attribute_view = form_autocomplete_attribute.has_value() ? form_autocomplete_attribute->utf16_view() : u""sv;
        if (form && form_autocomplete_attribute_view.equals_ignoring_ascii_case(u"off"sv)) {
            attr_details.field_name = "off"_utf16;
        }
        // Otherwise, set the element's autofill field name to "on".
        else {
            attr_details.field_name = "on"_utf16;
        }

        return attr_details;
    };

    // 1. If the element has no autocomplete attribute, then jump to the step labeled default.
    if (!autocomplete_element_to_html_element().has_attribute(AttributeNames::autocomplete))
        return step_default();

    // 2. Let tokens be the result of splitting the attribute's value on ASCII whitespace.
    auto autocomplete_value = autocomplete_element_to_html_element().attribute(AttributeNames::autocomplete);
    auto autocomplete_value_view = autocomplete_value.has_value() ? autocomplete_value->utf16_view() : u""sv;
    auto tokens = HTML::autocomplete_tokens(autocomplete_value_view);

    // 3. If tokens is empty, then jump to the step labeled default.
    if (tokens.is_empty())
        return step_default();

    // 4. Let index be the index of the last token in tokens.
    auto index = tokens.size() - 1;

    // 5. Let field be the indexth token in tokens.
    auto field = tokens[index];

    // 6. Set the category, maximum tokens pair to the result of determining a field's category given field.
    auto [category, maximum_tokens] = determine_a_field_category(field);

    // 7. If category is null, then jump to the step labeled default.
    if (!category.has_value())
        return step_default();

    // 8. If the number of tokens in tokens is greater than maximum tokens, then jump to the step labeled default.
    if (tokens.size() > maximum_tokens.value())
        return step_default();

    // 9. If category is Off or Automatic but the element's autocomplete attribute is wearing the autofill anchor mantle,
    //    then jump to the step labeled default.
    if ((category == Category::Off || category == Category::Automatic)
        && get_autofill_mantle() == AutofillMantle::Anchor)
        return step_default();

    // 10. If category is Off, set the element's autofill field name to the string "off", set its autofill hint set to empty,
    //     and set its IDL-exposed autofill value to the string "off". Then, return.
    if (category == Category::Off) {
        attr_details.field_name = "off"_utf16;
        attr_details.hint_set = {};
        attr_details.value = "off"_utf16;
        return attr_details;
    }

    // 11. If category is Automatic, set the element's autofill field name to the string "on", set its autofill hint set to empty,
    //     and set its IDL-exposed autofill value to the string "on". Then, return.
    if (category == Category::Automatic) {
        attr_details.field_name = "on"_utf16;
        attr_details.hint_set = {};
        attr_details.value = "on"_utf16;
        return attr_details;
    }

    // 12. Let scope tokens be an empty list.
    Vector<Utf16String> scope_tokens;

    // 13. Let hint tokens be an empty set.
    HashTable<Utf16FlyString> hint_tokens;

    // 14. Let credential type be null.
    Optional<Utf16FlyString> credential_type;

    // 15. Let IDL value have the same value as field.
    // NOTE: lowercasing is not mentioned in the spec, but required to pass all WPT tests.
    auto idl_value = field.to_ascii_lowercase();

    auto step_done = [&] {
        // 26. Done: Set the element's autofill hint set to hint tokens.
        attr_details.hint_set = hint_tokens.values();

        // 27. Set the element's non-autofill credential type to credential type.
        attr_details.credential_type = credential_type;

        // 28. Set the element's autofill scope to scope tokens.
        attr_details.scope = scope_tokens;

        // 29. Set the element's autofill field name to field.
        attr_details.field_name = Utf16String::from_utf16(field);

        // 30. Set the element's IDL-exposed autofill value to IDL value.
        attr_details.value = idl_value;

        // 31. Return.
        return attr_details;
    };

    // 16. If category is Credential and the indexth token in tokens is an ASCII case-insensitive match for "webauthn",
    //     then run the substeps that follow:
    if (category == Category::Credential && tokens[index].equals_ignoring_ascii_case(u"webauthn"sv)) {
        // 1. Set credential type to "webauthn".
        credential_type = AutocompleteToken::webauthn;

        // 2. If the indexth token in tokens is the first entry, then skip to the step labeled done.
        if (index == 0)
            return step_done();

        // 3. Decrement index by one.
        --index;

        // 4. Set the category, maximum tokens pair to the result of determining a field's category given the indexth token in tokens.
        auto category_and_maximum_tokens = determine_a_field_category(tokens[index]);
        category = category_and_maximum_tokens.category;
        maximum_tokens = category_and_maximum_tokens.maximum_tokens;

        // 5. If category is not Normal and category is not Contact, then jump to the step labeled default.
        if (category != Category::Normal && category != Category::Contact)
            return step_default();

        // 6. If index is greater than maximum tokens minus one (i.e. if the number of remaining tokens is greater than maximum tokens),
        //    then jump to the step labeled default.
        if (index > maximum_tokens.value() - 1)
            return step_default();

        // 7. Set IDL value to the concatenation of the indexth token in tokens, a U+0020 SPACE character, and the previous value of IDL value.
        idl_value = Utf16String::formatted("{} {}", tokens[index], idl_value);
    }

    // 17. If the indexth token in tokens is the first entry, then skip to the step labeled done.
    if (index == 0)
        return step_done();

    // 18. Decrement index by one.
    --index;

    // 19. If category is Contact and the indexth token in tokens is an ASCII case-insensitive match for one of the strings in the following list,
    //     then run the substeps that follow:
    if (auto contact = category == Category::Contact ? contact_token_canonicalized(tokens[index]) : Optional<Utf16FlyString> {}; contact.has_value()) {
        // 1. Let contact be the matching string from the list above.
        auto contact_string = contact->to_utf16_string();

        // 2. Insert contact at the start of scope tokens.
        scope_tokens.prepend(contact_string);

        // 3. Add contact to hint tokens.
        hint_tokens.set(*contact);

        // 4. Let IDL value be the concatenation of contact, a U+0020 SPACE character, and the previous value of IDL value.
        idl_value = Utf16String::formatted("{} {}", contact->view(), idl_value);

        // 5. If the indexth entry in tokens is the first entry, then skip to the step labeled done.
        if (index == 0)
            return step_done();

        // 6. Decrement index by one.
        --index;
    }

    // 20. If the indexth token in tokens is an ASCII case-insensitive match for one of the strings in the following list,
    //     then run the substeps that follow:
    if (auto mode = mode_token_canonicalized(tokens[index]); mode.has_value()) {
        // 1. Let mode be the matching string from the list above.
        auto mode_string = mode->to_utf16_string();

        // 2. Insert mode at the start of scope tokens.
        scope_tokens.prepend(mode_string);

        // 3. Add mode to hint tokens.
        hint_tokens.set(*mode);

        // 4. Let IDL value be the concatenation of mode, a U+0020 SPACE character, and the previous value of IDL value.
        idl_value = Utf16String::formatted("{} {}", mode->view(), idl_value);

        // 5. If the indexth entry in tokens is the first entry, then skip to the step labeled done.
        if (index == 0)
            return step_done();

        // 6. Decrement index by one.
        --index;
    }

    // 21. If the indexth entry in tokens is not the first entry, then jump to the step labeled default.
    if (index != 0)
        return step_default();

    // 22. If the first eight characters of the indexth token in tokens are not an ASCII case-insensitive match for the string "section-",
    //     then jump to the step labeled default.
    if (tokens[index].length_in_code_units() < 8 || !tokens[index].substring_view(0, 8).equals_ignoring_ascii_case(u"section-"sv))
        return step_default();

    // 23. Let section be the indexth token in tokens, converted to ASCII lowercase.
    auto section = tokens[index].to_ascii_lowercase();

    // 24. Insert section at the start of scope tokens.
    scope_tokens.prepend(section);

    // 25. Let IDL value be the concatenation of section, a U+0020 SPACE character, and the previous value of IDL value.
    idl_value = Utf16String::formatted("{} {}", section, idl_value);

    return step_done();
}

}
