/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

inline String string_from_rust_data(StyleValueFFI::RetainedString const& string)
{
    if (string.raw != 0)
        return String::from_raw(string.raw);
    return String::from_utf8_without_validation({ string.bytes, string.length });
}

// Marshals a URL's request URL modifiers for a Rust-owned allocation, retaining one leaked
// reference to each string-valued modifier.
inline Vector<StyleValueFFI::RetainedRequestUrlModifier> retain_url_modifiers_for_rust(URL const& url)
{
    Vector<StyleValueFFI::RetainedRequestUrlModifier> modifiers;
    modifiers.ensure_capacity(url.request_url_modifiers().size());
    for (auto const& modifier : url.request_url_modifiers()) {
        StyleValueFFI::RetainedRequestUrlModifier ffi_modifier { to_underlying(modifier.type()), 0, { 0, nullptr } };
        modifier.value().visit(
            [&](CrossOriginModifierValue value) { ffi_modifier.enum_value = to_underlying(value); },
            [&](ReferrerPolicyModifierValue value) { ffi_modifier.enum_value = to_underlying(value); },
            [&](Utf16FlyString const& string) { ffi_modifier.string_value.raw = string.to_raw_leaked(); });
        modifiers.unchecked_append(ffi_modifier);
    }
    return modifiers;
}

// Rebuilds a URL from the Rust-owned payload fields.
inline URL url_from_rust_data(StyleValueFFI::RetainedString const& url_string, u8 url_type, StyleValueFFI::RetainedRequestUrlModifierList const& modifier_list)
{
    Vector<RequestURLModifier> modifiers;
    modifiers.ensure_capacity(modifier_list.length);
    for (size_t i = 0; i < modifier_list.length; ++i) {
        auto const& modifier = modifier_list.pointer[i];
        switch (static_cast<RequestURLModifier::Type>(modifier.modifier_type)) {
        case RequestURLModifier::Type::CrossOrigin:
            modifiers.unchecked_append(RequestURLModifier::create_cross_origin(static_cast<CrossOriginModifierValue>(modifier.enum_value)));
            break;
        case RequestURLModifier::Type::Integrity:
            modifiers.unchecked_append(RequestURLModifier::create_integrity(Utf16FlyString::from_raw(modifier.string_value.raw)));
            break;
        case RequestURLModifier::Type::ReferrerPolicy:
            modifiers.unchecked_append(RequestURLModifier::create_referrer_policy(static_cast<ReferrerPolicyModifierValue>(modifier.enum_value)));
            break;
        }
    }
    return URL(string_from_rust_data(url_string), static_cast<URL::Type>(url_type), move(modifiers));
}

class URLStyleValue final : public StyleValueWithDefaultOperators<URLStyleValue> {
public:
    static ValueComparingNonnullRefPtr<URLStyleValue const> create(URL const& url)
    {
        return adopt_ref(*new (nothrow) URLStyleValue(url));
    }

    virtual ~URLStyleValue() override = default;

    URL url() const
    {
        auto const& data = m_value->url;
        return url_from_rust_data(data.url, data.url_type, data.modifiers);
    }

private:
    friend class StyleValue;

    explicit URLStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::URL, data)
    {
    }

    URLStyleValue(URL const& url)
        : StyleValueWithDefaultOperators(Type::URL, make_url_data(url))
    {
    }

    static StyleValueFFI::StyleValueData const* make_url_data(URL const& url)
    {
        // The Rust allocation takes ownership of one leaked reference to each retained string.
        auto modifiers = retain_url_modifiers_for_rust(url);
        auto url_string = url.url();
        auto url_bytes = url_string.bytes();
        return StyleValueFFI::rust_style_value_create_url(url_string.to_raw_leaked(), url_bytes.data(), url_bytes.size(), to_underlying(url.type()), modifiers.data(), modifiers.size());
    }
};

}
