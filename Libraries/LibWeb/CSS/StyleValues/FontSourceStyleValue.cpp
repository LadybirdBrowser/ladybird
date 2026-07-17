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

StyleValueFFI::StyleValueData* FontSourceStyleValue::make_font_source_data(Source const& source, Optional<Utf16FlyString> const& format, Vector<FontTech> const& tech)
{
    // The Rust allocation takes ownership of one strong reference to the local name, or one
    // leaked reference to each retained string.
    bool is_local = source.has<Local>();
    void const* local_name = nullptr;
    FlatPtr url_string = 0;
    u8 url_type = 0;
    Vector<StyleValueFFI::RetainedRequestUrlModifier> modifiers;
    if (is_local) {
        auto const& local = source.get<Local>();
        local.name->ref();
        local_name = local.name.ptr();
    } else {
        auto const& url = source.get<URL>();
        url_string = url.url().to_raw_leaked();
        url_type = to_underlying(url.type());
        modifiers = retain_url_modifiers_for_rust(url);
    }
    static_assert(sizeof(FontTech) == sizeof(u8));
    return StyleValueFFI::rust_style_value_create_font_source(
        is_local, local_name, url_string, url_type, modifiers.data(), modifiers.size(),
        format.has_value(), format.has_value() ? format->to_raw_leaked() : 0,
        reinterpret_cast<u8 const*>(tech.data()), tech.size());
}

FontSourceStyleValue::Source FontSourceStyleValue::source() const
{
    auto const& data = m_value->font_source;
    if (data.is_local)
        return Local { *static_cast<StyleValue const*>(data.local_name.pointer) };

    return url_from_rust_data(data.url, data.url_type, data.url_modifiers);
}

FontSourceStyleValue::FontSourceStyleValue(Source source, Optional<Utf16FlyString> format, Vector<FontTech> tech)
    : StyleValueWithDefaultOperators(Type::FontSource, make_font_source_data(source, format, tech))
{
}

FontSourceStyleValue::~FontSourceStyleValue() = default;

void FontSourceStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    // <font-src> = <url> [ format(<font-format>)]? [ tech( <font-tech>#)]? | local(<family-name>)
    source().visit(
        [&builder](Local const& local) {
            // local(<family-name>)

            // https://www.w3.org/TR/cssom-1/#serialize-a-local
            // To serialize a LOCAL means to create a string represented by "local(",
            // followed by the serialization of the LOCAL as a string, followed by ")".
            builder.append("local("sv);
            local.name->serialize(builder, SerializationMode::Normal);
            builder.append(')');
        },
        [this, &builder](URL const& url) {
            // <url> [ format(<font-format>)]? [ tech( <font-tech>#)]?
            builder.append(url.to_string());

            if (auto format = this->format(); format.has_value()) {
                builder.append(" format("sv);
                serialize_an_identifier(builder, *format);
                builder.append(")"sv);
            }

            if (auto tech_list = this->tech(); !tech_list.is_empty()) {
                builder.append(" tech("sv);
                serialize_a_comma_separated_list(builder, tech_list, [](auto& b, FontTech const tech) {
                    return b.append(CSS::to_string(tech));
                });
                builder.append(")"sv);
            }
        });
}

bool FontSourceStyleValue::properties_equal(FontSourceStyleValue const& other) const
{
    auto other_source = other.source();
    bool sources_equal = source().visit(
        [&other_source](Local const& local) {
            if (auto* other_local = other_source.get_pointer<Local>()) {
                return local.name == other_local->name;
            }
            return false;
        },
        [&other_source](URL const& url) {
            if (auto* other_url = other_source.get_pointer<URL>()) {
                return url == *other_url;
            }
            return false;
        });

    return sources_equal
        && format() == other.format()
        && tech() == other.tech();
}

}
