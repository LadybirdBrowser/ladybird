/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/Bindings/DOMTokenListPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace {

// https://infra.spec.whatwg.org/#set-append
inline void append_to_ordered_set(Vector<String>& set, String item)
{
    if (!set.contains_slow(item))
        set.append(move(item));
}

// https://infra.spec.whatwg.org/#list-remove
inline void remove_from_ordered_set(Vector<String>& set, StringView item)
{
    set.remove_first_matching([&](auto const& value) { return value == item; });
}

// https://infra.spec.whatwg.org/#set-replace
inline void replace_in_ordered_set(Vector<String>& set, String const& item, String replacement)
{
    auto item_index = set.find_first_index(item);
    VERIFY(item_index.has_value());

    auto replacement_index = set.find_first_index(replacement);
    if (!replacement_index.has_value()) {
        set[*item_index] = move(replacement);
        return;
    }

    auto index_to_set = min(*item_index, *replacement_index);
    auto index_to_remove = max(*item_index, *replacement_index);
    if (index_to_set == index_to_remove)
        return;

    set[index_to_set] = move(replacement);
    set.remove(index_to_remove);
}

}

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(DOMTokenList);

GC::Ref<DOMTokenList> DOMTokenList::create(Element& associated_element, FlyString associated_attribute)
{
    auto& realm = associated_element.realm();
    return realm.create<DOMTokenList>(associated_element, move(associated_attribute));
}

// https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A2
DOMTokenList::DOMTokenList(Element& associated_element, FlyString associated_attribute)
    : Bindings::PlatformObject(associated_element.realm())
    , m_associated_element(associated_element)
    , m_associated_attribute(move(associated_attribute))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = 1 };

    // When a DOMTokenList object set is created:
    // 1. Let element be set’s element.
    // 2. Let attributeName be set’s attribute name.
    // 3. Let value be the result of getting an attribute value given element and attributeName.
    auto value = m_associated_element->get_attribute_value(m_associated_attribute);

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

// https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A1
void DOMTokenList::associated_attribute_changed(StringView value)
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
Optional<String> DOMTokenList::item(size_t index) const
{
    // 1. If index is equal to or greater than this’s token set’s size, then return null.
    if (index >= m_token_set.size())
        return {};

    // 2. Return this’s token set[index].
    return m_token_set[index];
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-contains
bool DOMTokenList::contains(String const& token)
{
    return m_token_set.contains_slow(token);
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-add
WebIDL::ExceptionOr<void> DOMTokenList::add(Vector<String> const& tokens)
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

// https://dom.spec.whatwg.org/#dom-domtokenlist-remove
WebIDL::ExceptionOr<void> DOMTokenList::remove(Vector<String> const& tokens)
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
WebIDL::ExceptionOr<bool> DOMTokenList::toggle(String const& token, Optional<bool> force)
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
WebIDL::ExceptionOr<bool> DOMTokenList::replace(String const& token, String const& new_token)
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
WebIDL::ExceptionOr<bool> DOMTokenList::supports(StringView token)
{
    // 1. Let result be the return value of validation steps called with token.
    auto result = run_validation_steps(token);

    // 2. Return result.
    return result;
}

// https://dom.spec.whatwg.org/#concept-domtokenlist-validation
WebIDL::ExceptionOr<bool> DOMTokenList::run_validation_steps(StringView token)
{
    static HashMap<SupportedTokenKey, Vector<StringView>> supported_tokens_map = {
        // https://html.spec.whatwg.org/multipage/links.html#linkTypes
        { { HTML::TagNames::link, HTML::AttributeNames::rel },
            { "modulepreload"sv, "preload"sv, "preconnect"sv, "dns-prefetch"sv, "stylesheet"sv, "icon"sv, "alternate"sv, "prefetch"sv, "prerender"sv, "next"sv, "manifest"sv, "apple-touch-icon"sv, "apple-touch-icon-precomposed"sv, "canonical"sv } },
        { { HTML::TagNames::a, HTML::AttributeNames::rel },
            { "noreferrer"sv, "noopener"sv, "opener"sv } },
        { { HTML::TagNames::area, HTML::AttributeNames::rel },
            { "noreferrer"sv, "noopener"sv, "opener"sv } },
        { { HTML::TagNames::form, HTML::AttributeNames::rel },
            { "noreferrer"sv, "noopener"sv, "opener"sv } },

        // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#attr-iframe-sandbox
        { { HTML::TagNames::iframe, HTML::AttributeNames::sandbox },
            { "allow-downloads"sv, "allow-forms"sv, "allow-modals"sv, "allow-orientation-lock"sv, "allow-pointer-lock"sv, "allow-popups"sv, "allow-popups-to-escape-sandbox"sv, "allow-presentation"sv, "allow-same-origin"sv, "allow-scripts"sv, "allow-top-navigation"sv, "allow-top-navigation-by-user-activation"sv, "allow-top-navigation-to-custom-protocols"sv } },
    };

    // 1. If set’s element and attribute name does not define supported tokens, then throw a TypeError.
    auto supported_tokens = supported_tokens_map.get({ m_associated_element->local_name(), m_associated_attribute });
    if (!supported_tokens.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Attribute {} does not define any supported tokens", m_associated_attribute)) };

    // 2. Let lowercaseToken be token, in ASCII lowercase.
    auto lowercase_token = token.to_ascii_lowercase_string();

    // 3. If lowercaseToken is present in the supported tokens of set’s element and attribute name, then return true.
    if (supported_tokens->contains_slow(lowercase_token))
        return true;

    // 4. Return false.
    return false;
}

// https://dom.spec.whatwg.org/#concept-ordered-set-parser
Vector<String> DOMTokenList::parse_ordered_set(StringView input) const
{
    // 1. Let inputTokens be the result of splitting input on ASCII whitespace.
    auto split_values = input.split_view_if(Infra::is_ascii_whitespace);

    // 2. Let tokens be a new ordered set.
    Vector<String> tokens;

    // 3. For each token of inputTokens: append token to tokens.
    for (auto const& split_value : split_values)
        append_to_ordered_set(tokens, MUST(String::from_utf8(split_value)));

    // 4. Return tokens.
    return tokens;
}

// https://dom.spec.whatwg.org/#concept-ordered-set-serializer
String DOMTokenList::serialize_ordered_set() const
{
    // The ordered set serializer takes a set and returns the concatenation of set using U+0020 SPACE.
    return MUST(String::join(' ', m_token_set));
}

// https://dom.spec.whatwg.org/#dom-domtokenlist-value
String DOMTokenList::value() const
{
    return m_associated_element->get_attribute_value(m_associated_attribute);
}

// https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-set-value%E2%91%A2
void DOMTokenList::set_value(String const& value)
{
    GC::Ptr<DOM::Element> associated_element = m_associated_element.ptr();
    if (!associated_element)
        return;

    associated_element->set_attribute_value(m_associated_attribute, value);
}

WebIDL::ExceptionOr<void> DOMTokenList::validate_token(StringView token) const
{
    TRY(validate_token_not_empty(token));
    TRY(validate_token_not_whitespace(token));
    return {};
}

WebIDL::ExceptionOr<void> DOMTokenList::validate_token_not_empty(StringView token) const
{
    if (token.is_empty())
        return WebIDL::SyntaxError::create(realm(), "Non-empty DOM tokens are not allowed"_utf16);
    return {};
}

WebIDL::ExceptionOr<void> DOMTokenList::validate_token_not_whitespace(StringView token) const
{
    if (any_of(token, Infra::is_ascii_whitespace))
        return WebIDL::InvalidCharacterError::create(realm(), "DOM tokens containing ASCII whitespace are not allowed"_utf16);
    return {};
}

// https://dom.spec.whatwg.org/#concept-dtl-update
void DOMTokenList::run_update_steps()
{
    GC::Ptr<DOM::Element> associated_element = m_associated_element.ptr();
    if (!associated_element)
        return;

    // 1. If get an attribute by namespace and local name given null, set’s attribute name, and set’s element returns null and set’s token set is empty, then return.
    auto attribute = associated_element->get_attribute_ns({}, m_associated_attribute);
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
