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

Vector<String> AutocompleteElement::autocomplete_tokens() const
{
    auto autocomplete_value = autocomplete_element_to_html_element().attribute(AttributeNames::autocomplete).value_or({});

    Vector<String> autocomplete_tokens;
    for (auto& token : autocomplete_value.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace))
        autocomplete_tokens.append(MUST(String::from_utf8(token)));
    return autocomplete_tokens;
}

String AutocompleteElement::autocomplete() const
{
    // The autocomplete IDL attribute, on getting, must return the element's IDL-exposed autofill value.
    auto details = parse_autocomplete_attribute();
    return details.value;
}

WebIDL::ExceptionOr<void> AutocompleteElement::set_autocomplete(String const& value)
{
    // The autocomplete IDL attribute [...] on setting, must reflect the content attribute of the same name.
    TRY(autocomplete_element_to_html_element().set_attribute(AttributeNames::autocomplete, value));
    return {};
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
static CategoryAndMaximumTokens determine_a_field_category(StringView const& field)
{
#define CASE_CATEGORY(token, maximum_number_of_tokens, category) \
    if (field.equals_ignoring_ascii_case(token))                 \
        return CategoryAndMaximumTokens { Category::category, maximum_number_of_tokens };

    // 1. If the field is not an ASCII case-insensitive match for one of the tokens given
    //    in the first column of the following table, return the pair (null, null).
    // 2. Otherwise, let maximum tokens and category be the values of the cells in the second
    //    and third columns of that row respectively.
    // 3. Return the pair (category, maximum tokens).
    CASE_CATEGORY("off"sv, 1, Off);
    CASE_CATEGORY("on"sv, 1, Automatic);
    CASE_CATEGORY("name"sv, 3, Normal);
    CASE_CATEGORY("honorific-prefix"sv, 3, Normal);
    CASE_CATEGORY("given-name"sv, 3, Normal);
    CASE_CATEGORY("additional-name"sv, 3, Normal);
    CASE_CATEGORY("family-name"sv, 3, Normal);
    CASE_CATEGORY("honorific-suffix"sv, 3, Normal);
    CASE_CATEGORY("nickname"sv, 3, Normal);
    CASE_CATEGORY("organization-title"sv, 3, Normal);
    CASE_CATEGORY("username"sv, 3, Normal);
    CASE_CATEGORY("new-password"sv, 3, Normal);
    CASE_CATEGORY("current-password"sv, 3, Normal);
    CASE_CATEGORY("one-time-code"sv, 3, Normal);
    CASE_CATEGORY("organization"sv, 3, Normal);
    CASE_CATEGORY("street-address"sv, 3, Normal);
    CASE_CATEGORY("address-line1"sv, 3, Normal);
    CASE_CATEGORY("address-line2"sv, 3, Normal);
    CASE_CATEGORY("address-line3"sv, 3, Normal);
    CASE_CATEGORY("address-level4"sv, 3, Normal);
    CASE_CATEGORY("address-level3"sv, 3, Normal);
    CASE_CATEGORY("address-level2"sv, 3, Normal);
    CASE_CATEGORY("address-level1"sv, 3, Normal);
    CASE_CATEGORY("country"sv, 3, Normal);
    CASE_CATEGORY("country-name"sv, 3, Normal);
    CASE_CATEGORY("postal-code"sv, 3, Normal);
    CASE_CATEGORY("cc-name"sv, 3, Normal);
    CASE_CATEGORY("cc-given-name"sv, 3, Normal);
    CASE_CATEGORY("cc-additional-name"sv, 3, Normal);
    CASE_CATEGORY("cc-family-name"sv, 3, Normal);
    CASE_CATEGORY("cc-number"sv, 3, Normal);
    CASE_CATEGORY("cc-exp"sv, 3, Normal);
    CASE_CATEGORY("cc-exp-month"sv, 3, Normal);
    CASE_CATEGORY("cc-exp-year"sv, 3, Normal);
    CASE_CATEGORY("cc-csc"sv, 3, Normal);
    CASE_CATEGORY("cc-type"sv, 3, Normal);
    CASE_CATEGORY("transaction-currency"sv, 3, Normal);
    CASE_CATEGORY("transaction-amount"sv, 3, Normal);
    CASE_CATEGORY("language"sv, 3, Normal);
    CASE_CATEGORY("bday"sv, 3, Normal);
    CASE_CATEGORY("bday-day"sv, 3, Normal);
    CASE_CATEGORY("bday-month"sv, 3, Normal);
    CASE_CATEGORY("bday-year"sv, 3, Normal);
    CASE_CATEGORY("sex"sv, 3, Normal);
    CASE_CATEGORY("url"sv, 3, Normal);
    CASE_CATEGORY("photo"sv, 3, Normal);
    CASE_CATEGORY("tel"sv, 4, Contact);
    CASE_CATEGORY("tel-country-code"sv, 4, Contact);
    CASE_CATEGORY("tel-national"sv, 4, Contact);
    CASE_CATEGORY("tel-area-code"sv, 4, Contact);
    CASE_CATEGORY("tel-local"sv, 4, Contact);
    CASE_CATEGORY("tel-local-prefix"sv, 4, Contact);
    CASE_CATEGORY("tel-local-suffix"sv, 4, Contact);
    CASE_CATEGORY("tel-extension"sv, 4, Contact);
    CASE_CATEGORY("email"sv, 4, Contact);
    CASE_CATEGORY("impp"sv, 4, Contact);
    CASE_CATEGORY("webauthn"sv, 5, Credential);

#undef CASE_CATEGORY

    return CategoryAndMaximumTokens {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#autofill-processing-model
AutocompleteElement::AttributeDetails AutocompleteElement::parse_autocomplete_attribute() const
{
    AttributeDetails attr_details {};

    auto step_default = [&] {
        // 32. Default: Let the element's IDL-exposed autofill value be the empty string, and its autofill hint set and autofill scope be empty.
        attr_details.value = {};
        attr_details.hint_set = {};
        attr_details.scope = {};

        // 33. If the element's autocomplete attribute is wearing the autofill anchor mantle,
        //     then let the element's autofill field name be the empty string and return.
        if (get_autofill_mantle() == AutofillMantle::Anchor) {
            attr_details.field_name = {};
            return attr_details;
        }

        // 34. Let form be the element's form owner, if any, or null otherwise.
        auto const* form = as<FormAssociatedElement const>(autocomplete_element_to_html_element()).form();

        // 35. If form is not null and form's autocomplete attribute is in the off state, then let the element's autofill field name be "off".
        if (form && form->attribute(AttributeNames::autocomplete) == idl_enum_to_string(Bindings::Autocomplete::Off)) {
            attr_details.field_name = "off"_string;
        }
        // Otherwise, let the element's autofill field name be "on".
        else {
            attr_details.field_name = "on"_string;
        }

        return attr_details;
    };

    // 1. If the element has no autocomplete attribute, then jump to the step labeled default.
    if (!autocomplete_element_to_html_element().has_attribute(AttributeNames::autocomplete))
        return step_default();

    // 2. Let tokens be the result of splitting the attribute's value on ASCII whitespace.
    auto tokens = autocomplete_tokens();

    // 3. If tokens is empty, then jump to the step labeled default.
    if (tokens.is_empty())
        return step_default();

    // 4. Let index be the index of the last token in tokens.
    auto index = tokens.size() - 1;

    // 5. Let field be the indexth token in tokens.
    auto const& field = tokens[index];

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

    // 10. If category is Off, let the element's autofill field name be the string "off", let its autofill hint set be empty,
    //     and let its IDL-exposed autofill value be the string "off". Then, return.
    if (category == Category::Off) {
        attr_details.field_name = "off"_string;
        attr_details.hint_set = {};
        attr_details.value = "off"_string;
        return attr_details;
    }

    // 11. If category is Automatic, let the element's autofill field name be the string "on", let its autofill hint set be empty,
    //     and let its IDL-exposed autofill value be the string "on". Then, return.
    if (category == Category::Automatic) {
        attr_details.field_name = "on"_string;
        attr_details.hint_set = {};
        attr_details.value = "on"_string;
        return attr_details;
    }

    // 12. Let scope tokens be an empty list.
    Vector<String> scope_tokens;

    // 13. Let hint tokens be an empty set.
    HashTable<String> hint_tokens;

    // 14. Let credential type be null.
    Optional<String> credential_type;

    // 15. Let IDL value have the same value as field.
    // NOTE: lowercasing is not mentioned in the spec, but required to pass all WPT tests.
    auto idl_value = field.to_ascii_lowercase();

    auto step_done = [&] {
        // 26. Done: Let the element's autofill hint set be hint tokens.
        attr_details.hint_set = hint_tokens.values();

        // 27. Let the element's non-autofill credential type be credential type.
        attr_details.credential_type = credential_type;

        // 28. Let the element's autofill scope be scope tokens.
        attr_details.scope = scope_tokens;

        // 29. Let the element's autofill field name be field.
        attr_details.field_name = field;

        // 30. Let the element's IDL-exposed autofill value be IDL value.
        attr_details.value = idl_value;

        // 31. Return.
        return attr_details;
    };

    // 16. If category is Credential and the indexth token in tokens is an ASCII case-insensitive match for "webauthn",
    //     then run the substeps that follow:
    if (category == Category::Credential && tokens[index].equals_ignoring_ascii_case("webauthn"sv)) {
        // 1. Set credential type to "webauthn".
        credential_type = "webauthn"_string;

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
        idl_value = MUST(String::formatted("{} {}", tokens[index], idl_value));
    }

    // 17. If the indexth token in tokens is the first entry, then skip to the step labeled done.
    if (index == 0)
        return step_done();

    // 18. Decrement index by one.
    --index;

    // 19. If category is Contact and the indexth token in tokens is an ASCII case-insensitive match for one of the strings in the following list,
    //     then run the substeps that follow:
    if (category == Category::Contact && tokens[index].to_ascii_lowercase().is_one_of("home", "work", "mobile", "fax", "pager")) {
        // 1. Let contact be the matching string from the list above.
        auto contact = tokens[index].to_ascii_lowercase();

        // 2. Insert contact at the start of scope tokens.
        scope_tokens.prepend(contact);

        // 3. Add contact to hint tokens.
        hint_tokens.set(contact);

        // 4. Let IDL value be the concatenation of contact, a U+0020 SPACE character, and the previous value of IDL value.
        idl_value = MUST(String::formatted("{} {}", contact, idl_value));

        // 5. If the indexth entry in tokens is the first entry, then skip to the step labeled done.
        if (index == 0)
            return step_done();

        // 6. Decrement index by one.
        --index;
    }

    // 20. If the indexth token in tokens is an ASCII case-insensitive match for one of the strings in the following list,
    //     then run the substeps that follow:
    if (tokens[index].to_ascii_lowercase().is_one_of("shipping", "billing")) {
        // 1. Let mode be the matching string from the list above.
        auto mode = tokens[index].to_ascii_lowercase();

        // 2. Insert mode at the start of scope tokens.
        scope_tokens.prepend(mode);

        // 3. Add mode to hint tokens.
        hint_tokens.set(mode);

        // 4. Let IDL value be the concatenation of mode, a U+0020 SPACE character, and the previous value of IDL value.
        idl_value = MUST(String::formatted("{} {}", mode, idl_value));

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
    if (!tokens[index].to_ascii_lowercase().starts_with_bytes("section-"sv))
        return step_default();

    // 23. Let section be the indexth token in tokens, converted to ASCII lowercase.
    auto section = tokens[index].to_ascii_lowercase();

    // 24. Insert section at the start of scope tokens.
    scope_tokens.prepend(section);

    // 25. Let IDL value be the concatenation of section, a U+0020 SPACE character, and the previous value of IDL value.
    idl_value = MUST(String::formatted("{} {}", section, idl_value));

    return step_done();
}

}
