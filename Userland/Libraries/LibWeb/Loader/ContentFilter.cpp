/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/Loader/ContentFilter.h>

namespace Web {

ContentFilter& ContentFilter::the()
{
    static ContentFilter filter;
    return filter;
}

ContentFilter::ContentFilter() = default;

ContentFilter::~ContentFilter() = default;

bool ContentFilter::is_filtered(const URL::URL& url) const
{
    if (url.scheme() == "data")
        return false;

    auto url_string = url.to_byte_string();

    for (auto& pattern : m_patterns) {
        if (url_string.find(pattern.text).has_value())
            return true;
    }
    return false;
}

ErrorOr<void> ContentFilter::set_patterns(ReadonlySpan<String> patterns)
{
    m_patterns.clear_with_capacity();

    for (auto const& pattern : patterns) {
        TRY(m_patterns.try_empend(pattern));
    }

    return {};
}

}
