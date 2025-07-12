/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>

namespace Web::CSS {

FontSourceStyleValue::FontSourceStyleValue(Source source, Optional<FlyString> format, Vector<FontTech> tech)
    : StyleValueWithDefaultOperators(Type::FontSource)
    , m_source(move(source))
    , m_format(move(format))
    , m_tech(move(tech))
{
}

FontSourceStyleValue::~FontSourceStyleValue() = default;

String FontSourceStyleValue::to_string(SerializationMode) const
{
    // <font-src> = <url> [ format(<font-format>)]? [ tech( <font-tech>#)]? | local(<family-name>)
    return m_source.visit(
        [](Local const& local) {
            // local(<family-name>)

            // https://www.w3.org/TR/cssom-1/#serialize-a-local
            // To serialize a LOCAL means to create a string represented by "local(",
            // followed by the serialization of the LOCAL as a string, followed by ")".
            StringBuilder builder;
            builder.append("local("sv);
            builder.append(local.name->to_string(SerializationMode::Normal));
            builder.append(')');
            return builder.to_string_without_validation();
        },
        [this](URL const& url) {
            // <url> [ format(<font-format>)]? [ tech( <font-tech>#)]?
            StringBuilder builder;
            builder.append(url.to_string());

            if (m_format.has_value()) {
                builder.append(" format("sv);
                serialize_an_identifier(builder, *m_format);
                builder.append(")"sv);
            }

            if (!m_tech.is_empty()) {
                builder.append(" tech("sv);
                serialize_a_comma_separated_list(builder, m_tech, [](auto& builder, FontTech const tech) {
                    return builder.append(CSS::to_string(tech));
                });
                builder.append(")"sv);
            }

            return builder.to_string_without_validation();
        });
}

bool FontSourceStyleValue::properties_equal(FontSourceStyleValue const& other) const
{
    bool sources_equal = m_source.visit(
        [&other](Local const& local) {
            if (auto* other_local = other.m_source.get_pointer<Local>()) {
                return local.name == other_local->name;
            }
            return false;
        },
        [&other](URL const& url) {
            if (auto* other_url = other.m_source.get_pointer<URL>()) {
                return url == *other_url;
            }
            return false;
        });

    return sources_equal
        && m_format == other.m_format;
}

}
