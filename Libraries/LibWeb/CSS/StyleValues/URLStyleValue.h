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

inline Utf16View url_text_from_rust_data(StyleValueFFI::RetainedString const& string)
{
    auto view = StyleValueFFI::rust_css_url_text_view(&string);
    if (view.ascii)
        return Utf16View { StringView { reinterpret_cast<char const*>(view.ascii), view.length } };
    return Utf16View { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

// Marshals a URL's request URL modifiers for a Rust-owned allocation, retaining one leaked
// reference to each string-valued modifier.
inline Vector<StyleValueFFI::FfiRequestUrlModifier> retain_url_modifiers_for_rust(URL const& url)
{
    Vector<StyleValueFFI::FfiRequestUrlModifier> modifiers;
    modifiers.ensure_capacity(url.request_url_modifiers().size());
    for (auto const& modifier : url.request_url_modifiers()) {
        StyleValueFFI::FfiRequestUrlModifier ffi_modifier { to_underlying(modifier.type()), 0, 0 };
        modifier.value().visit(
            [&](CrossOriginModifierValue value) { ffi_modifier.enum_value = to_underlying(value); },
            [&](ReferrerPolicyModifierValue value) { ffi_modifier.enum_value = to_underlying(value); },
            [&](Utf16FlyString const& string) { ffi_modifier.string_value = string.to_raw_leaked(); });
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
            modifiers.unchecked_append(RequestURLModifier::create_integrity(css_string_from_rust(&modifier.string_value)));
            break;
        case RequestURLModifier::Type::ReferrerPolicy:
            modifiers.unchecked_append(RequestURLModifier::create_referrer_policy(static_cast<ReferrerPolicyModifierValue>(modifier.enum_value)));
            break;
        }
    }
    return URL(url_text_from_rust_data(url_string), static_cast<URL::Type>(url_type), move(modifiers));
}

class URLStyleValue final : public StyleValueWithDefaultOperators<URLStyleValue> {
public:
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
};

}
