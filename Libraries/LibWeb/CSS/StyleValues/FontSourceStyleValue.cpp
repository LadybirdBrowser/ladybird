/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>

namespace Web::CSS {

StyleValueFFI::StyleValueData const* FontSourceStyleValue::make_font_source_data(Source const& source, Optional<Utf16FlyString> const& format, Vector<FontTech> const& tech)
{
    // The Rust allocation takes ownership of one strong reference to the local name, or one
    // leaked reference to each retained string.
    bool is_local = source.has<Local>();
    StyleValueFFI::StyleValueData const* local_name = nullptr;
    String retained_url_string;
    FlatPtr url_string = 0;
    ReadonlyBytes url_bytes;
    u8 url_type = 0;
    Vector<StyleValueFFI::RetainedRequestUrlModifier> modifiers;
    if (is_local) {
        auto const& local = source.get<Local>();
        local_name = StyleValueFFI::rust_style_value_retain(local.name->rust_style_value_data());
    } else {
        auto const& url = source.get<URL>();
        retained_url_string = url.url();
        url_bytes = retained_url_string.bytes();
        url_string = retained_url_string.to_raw_leaked();
        url_type = to_underlying(url.type());
        modifiers = retain_url_modifiers_for_rust(url);
    }
    static_assert(sizeof(FontTech) == sizeof(u8));
    return StyleValueFFI::rust_style_value_create_font_source(
        is_local, local_name, url_string, url_bytes.data(), url_bytes.size(), url_type, modifiers.data(), modifiers.size(),
        format.has_value(), format.has_value() ? format->to_raw_leaked() : 0,
        reinterpret_cast<u8 const*>(tech.data()), tech.size());
}

FontSourceStyleValue::Source FontSourceStyleValue::source() const
{
    auto const& data = m_value->font_source;
    if (data.is_local)
        return Local { wrap_rust_child(data.local_name) };

    return url_from_rust_data(data.url, data.url_type, data.url_modifiers);
}

FontSourceStyleValue::FontSourceStyleValue(Source source, Optional<Utf16FlyString> format, Vector<FontTech> tech)
    : StyleValueWithDefaultOperators(Type::FontSource, make_font_source_data(source, format, tech))
{
}

FontSourceStyleValue::FontSourceStyleValue(StyleValueFFI::StyleValueData const* data)
    : StyleValueWithDefaultOperators(Type::FontSource, data)
{
}

FontSourceStyleValue::~FontSourceStyleValue() = default;

}
