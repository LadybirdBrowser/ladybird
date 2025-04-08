/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/URL.h>

namespace Web::CSS {

URL::URL(String url)
    : m_url(move(url))
{
}

// https://drafts.csswg.org/cssom-1/#serialize-a-url
String URL::to_string() const
{
    // To serialize a URL means to create a string represented by "url(", followed by the serialization of the URL as a string, followed by ")".
    StringBuilder builder;
    builder.append("url("sv);
    serialize_a_string(builder, m_url);
    builder.append(')');

    return builder.to_string_without_validation();
}

bool URL::operator==(URL const&) const = default;

}
