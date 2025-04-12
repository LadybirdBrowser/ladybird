/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-4/#urls
class URL {
public:
    URL(String url);

    String const& url() const { return m_url; }

    String to_string() const;
    bool operator==(URL const&) const;

private:
    String m_url;
};

}
