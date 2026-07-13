/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/Bindings/DOMTokenList.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace {

// https://infra.spec.whatwg.org/#set-append
inline void append_to_ordered_set(Vector<Utf16String>& set, Utf16View item)
{
    if (!set.contains_slow(item))
        set.append(Utf16String::from_utf16(item));
}

// https://infra.spec.whatwg.org/#list-remove
inline void remove_from_ordered_set(Vector<Utf16String>& set, Utf16View item)
{
    set.remove_first_matching([&](auto const& value) { return value == item; });
}

// https://infra.spec.whatwg.org/#set-replace
inline void replace_in_ordered_set(Vector<Utf16String>& set, Utf16View item, Utf16View replacement)
{
    auto item_index = set.find_first_index_if([&](auto const& value) { return value == item; });
    VERIFY(item_index.has_value());

    auto replacement_index = set.find_first_index_if([&](auto const& value) { return value == replacement; });
    if (!replacement_index.has_value()) {
        set[*item_index] = Utf16String::from_utf16(replacement);
        return;
    }

    if (*item_index == *replacement_index)
        return;

    if (*replacement_index < *item_index) {
        set.remove(*item_index);
        return;
    }

    set[*item_index] = move(set[*replacement_index]);
    set.remove(*replacement_index);
}

}

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(DOMTokenList);

GC::Ref<DOMTokenList> DOMTokenList::create(Element& associated_element, Utf16FlyString associated_attribute)
{
    auto& realm = associated_element.realm();
    return realm.create<DOMTokenList>(associated_element, move(associated_attribute));
}

// https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A2
DOMTokenList::DOMTokenList(Element& associated_element, Utf16FlyString associated_attribute)
    : Bindings::PlatformObject(associated_element.realm())
    , m_associated_element(associated_element)
    , m_associated_attribute(move(associated_attribute))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = 1 };

    // When a DOMTokenList object set is created:
    // 1. Let element be set’s element.
    // 2. Let attributeName be set’s attribute name.
    // 3. Let value be the result of getting an attribute value given element and attributeName.
    auto value = m_associated_element->get_attribute_value_view(m_associated_attribute).value_or({});

    // 4. Run the attribute change steps for element, attributeName, value, value, and null.
    associated_attribute_changed(value);
}

void DOMTokenList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMTokenList);
    Base::initialize(realm);
}

void DOMTokenList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_element);
}

size_t DOMTokenList::external_memory_size() const
{
    auto size = Base::external_memory_size();
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_token_set));
    for (auto const& token : m_token_set)
        size = JS::saturating_add_external_memory_size(size, JS::utf16_string_external_memory_size(token));
    return size;
}

// https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A1
void DOMTokenList::associated_attribute_changed(Utf16View value)
{
    // 1. If localName is set’s attribute name, namespace is null, and value is null, then empty token set.
    // 2. Otherwise, if localName is set’s attribute name and namespace is null, then set set’s token set to value,
    //    parsed.
    // AD-HOC: The caller is responsible for checking the name and namespace.
    m_token_set.clear();
    if (value.is_empty())
        return;
    m_token_set = parse_ordered_set(value);
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-item
Optional<Utf16String> DOMTokenList::item(size_t index) const
{
    // 1. If index is equal to or greater than this’s token set’s size, then return null.
    if (index >= m_token_set.size())
        return {};

    // 2. Return this’s token set[index].
    return m_token_set[index];
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-contains
bool DOMTokenList::contains(Utf16View token)
{
    return m_token_set.contains_slow(token);
}

WebIDL::ExceptionOr<void> DOMTokenList::add(Utf16View token)
{
    TRY(validate_token(token));
    append_to_ordered_set(m_token_set, token);
    run_update_steps();
    return {};
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-add
WebIDL::ExceptionOr<void> DOMTokenList::add(Vector<Utf16String> const& tokens)
{
    // 1. For each token of tokens:
    for (auto const& token : tokens) {
        // a. If token is the empty string, then throw a "SyntaxError" DOMException.
        // b. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
        TRY(validate_token(token));

        // 2. For each token of tokens, append token to this’s token set.
        append_to_ordered_set(m_token_set, token);
    }

    // 3. Run the update steps.
    run_update_steps();
    return {};
}

WebIDL::ExceptionOr<void> DOMTokenList::remove(Utf16View token)
{
    TRY(validate_token(token));
    remove_from_ordered_set(m_token_set, token);
    run_update_steps();
    return {};
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-remove
WebIDL::ExceptionOr<void> DOMTokenList::remove(Vector<Utf16String> const& tokens)
{
    // 1. For each token of tokens:
    for (auto const& token : tokens) {
        // a. If token is the empty string, then throw a "SyntaxError" DOMException.
        // b. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
        TRY(validate_token(token));

        // 2. For each token of tokens, remove token from this’s token set.
        remove_from_ordered_set(m_token_set, token);
    }

    // 3. Run the update steps.
    run_update_steps();
    return {};
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-toggle
WebIDL::ExceptionOr<bool> DOMTokenList::toggle(Utf16View token, Optional<bool> force)
{
    // 1. If token is the empty string, then throw a "SyntaxError" DOMException.
    // 2. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
    TRY(validate_token(token));

    // 3. If this’s token set[token] exists, then:
    if (contains(token)) {
        // a. If force is either not given or is false, then remove token from this’s token set, run the update steps and return false.
        if (!force.has_value() || !force.value()) {
            remove_from_ordered_set(m_token_set, token);
            run_update_steps();
            return false;
        }

        // b. Return true.
        return true;
    }

    // 4. Otherwise, if force not given or is true, append token to this’s token set, run the update steps, and return true.
    if (!force.has_value() || force.value()) {
        append_to_ordered_set(m_token_set, token);
        run_update_steps();
        return true;
    }

    // 5. Return false.
    return false;
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-replace
WebIDL::ExceptionOr<bool> DOMTokenList::replace(Utf16View token, Utf16View new_token)
{
    // 1. If either token or newToken is the empty string, then throw a "SyntaxError" DOMException.
    TRY(validate_token_not_empty(token));
    TRY(validate_token_not_empty(new_token));

    // 2. If either token or newToken contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
    TRY(validate_token_not_whitespace(token));
    TRY(validate_token_not_whitespace(new_token));

    // 3. If this’s token set does not contain token, then return false.
    if (!contains(token))
        return false;

    // 4. Replace token in this’s token set with newToken.
    replace_in_ordered_set(m_token_set, token, new_token);

    // 5. Run the update steps.
    run_update_steps();

    // 6. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-supports
WebIDL::ExceptionOr<bool> DOMTokenList::supports(Utf16View token)
{
    // 1. Let result be the return value of validation steps called with token.
    auto result = run_validation_steps(token);

    // 2. Return result.
    return result;
}

// https://dom.spec.whatwg.org/#concept-domtokenlist-validation
WebIDL::ExceptionOr<bool> DOMTokenList::run_validation_steps(Utf16View token)
{
    static NeverDestroyed<HashMap<SupportedTokenKey, Vector<Utf16View>>> supported_tokens_map { HashMap<SupportedTokenKey, Vector<Utf16View>> {
        // https://html.spec.whatwg.org/multipage/links.html#linkTypes
        { { HTML::TagNames::link, HTML::AttributeNames::rel },
            { u"modulepreload"sv, u"preload"sv, u"preconnect"sv, u"dns-prefetch"sv, u"stylesheet"sv, u"icon"sv, u"alternate"sv, u"prefetch"sv, u"prerender"sv, u"next"sv, u"manifest"sv, u"apple-touch-icon"sv, u"apple-touch-icon-precomposed"sv, u"canonical"sv } },
        { { HTML::TagNames::a, HTML::AttributeNames::rel },
            { u"noreferrer"sv, u"noopener"sv, u"opener"sv } },
        { { HTML::TagNames::area, HTML::AttributeNames::rel },
            { u"noreferrer"sv, u"noopener"sv, u"opener"sv } },
        { { HTML::TagNames::form, HTML::AttributeNames::rel },
            { u"noreferrer"sv, u"noopener"sv, u"opener"sv } },

        // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#attr-iframe-sandbox
        { { HTML::TagNames::iframe, HTML::AttributeNames::sandbox },
            { u"allow-downloads"sv, u"allow-forms"sv, u"allow-modals"sv, u"allow-orientation-lock"sv, u"allow-pointer-lock"sv, u"allow-popups"sv, u"allow-popups-to-escape-sandbox"sv, u"allow-presentation"sv, u"allow-same-origin"sv, u"allow-scripts"sv, u"allow-top-navigation"sv, u"allow-top-navigation-by-user-activation"sv, u"allow-top-navigation-to-custom-protocols"sv } },
    } };

    // 1. If set’s element and attribute name does not define supported tokens, then throw a TypeError.
    auto supported_tokens = supported_tokens_map->get({ m_associated_element->local_name(), m_associated_attribute });
    if (!supported_tokens.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Attribute {} does not define any supported tokens", m_associated_attribute) };

    // 2. Let lowercaseToken be token, in ASCII lowercase.
    // 3. If lowercaseToken is present in the supported tokens of set’s element and attribute name, then return true.
    for (auto supported_token : *supported_tokens) {
        if (token.equals_ignoring_ascii_case(supported_token))
            return true;
    }

    // 4. Return false.
    return false;
}

// https://dom.spec.whatwg.org/#concept-ordered-set-parser
Vector<Utf16String> DOMTokenList::parse_ordered_set(Utf16View input) const
{
    // 1. Let inputTokens be the result of splitting input on ASCII whitespace.
    // 2. Let tokens be a new ordered set.
    Vector<Utf16String> tokens;

    // 3. For each token of inputTokens: append token to tokens.
    Optional<size_t> token_start;
    for (size_t i = 0; i < input.length_in_code_units(); ++i) {
        if (Infra::is_ascii_whitespace(input.code_unit_at(i))) {
            if (token_start.has_value()) {
                append_to_ordered_set(tokens, input.substring_view(*token_start, i - *token_start));
                token_start.clear();
            }
            continue;
        }

        if (!token_start.has_value())
            token_start = i;
    }
    if (token_start.has_value())
        append_to_ordered_set(tokens, input.substring_view(*token_start));

    // 4. Return tokens.
    return tokens;
}

// https://dom.spec.whatwg.org/#concept-ordered-set-serializer
Utf16String DOMTokenList::serialize_ordered_set() const
{
    // The ordered set serializer takes a set and returns the concatenation of set using U+0020 SPACE.
    Utf16StringBuilder builder;
    for (auto const& token : m_token_set) {
        if (!builder.is_empty())
            builder.append_code_unit(' ');
        builder.append(token);
    }
    return builder.to_string();
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-value
Utf16String DOMTokenList::value() const
{
    return m_associated_element->get_attribute_value(m_associated_attribute);
}

// https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-set-value%E2%91%A2
void DOMTokenList::set_value(Utf16View value)
{
    GC::Ptr<DOM::Element> associated_element = m_associated_element.ptr();
    if (!associated_element)
        return;

    associated_element->set_attribute_value(m_associated_attribute, value);
}

WebIDL::ExceptionOr<void> DOMTokenList::validate_token(Utf16View token) const
{
    TRY(validate_token_not_empty(token));
    TRY(validate_token_not_whitespace(token));
    return {};
}

WebIDL::ExceptionOr<void> DOMTokenList::validate_token_not_empty(Utf16View token) const
{
    if (token.is_empty())
        return WebIDL::SyntaxError::create(realm(), "Non-empty DOM tokens are not allowed"_utf16);
    return {};
}

WebIDL::ExceptionOr<void> DOMTokenList::validate_token_not_whitespace(Utf16View token) const
{
    for (size_t i = 0; i < token.length_in_code_units(); ++i) {
        if (Infra::is_ascii_whitespace(token.code_unit_at(i)))
            return WebIDL::InvalidCharacterError::create(realm(), "DOM tokens containing ASCII whitespace are not allowed"_utf16);
    }
    return {};
}

// https://dom.spec.whatwg.org/#concept-dtl-update
void DOMTokenList::run_update_steps()
{
    GC::Ptr<DOM::Element> associated_element = m_associated_element.ptr();
    if (!associated_element)
        return;

    // 1. If get an attribute by namespace and local name given null, set’s attribute name, and set’s element returns null and set’s token set is empty, then return.
    auto attribute = associated_element->get_attribute_ns(Optional<Utf16FlyString> {}, m_associated_attribute);
    if (!attribute.has_value() && m_token_set.is_empty())
        return;

    // 2. Set an attribute value given set’s element, set’s attribute name, and the result of running the ordered set
    //    serializer for set’s token set.
    associated_element->set_attribute_value(m_associated_attribute, serialize_ordered_set());
}

Optional<JS::Value> DOMTokenList::item_value(size_t index) const
{
    auto string = item(index);
    if (!string.has_value())
        return {};
    return JS::PrimitiveString::create(vm(), string.release_value());
}

}
