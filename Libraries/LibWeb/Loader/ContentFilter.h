/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>

namespace Web {

class ContentFilter {
public:
    static ContentFilter& the();

    bool is_filtered(const URL::URL&) const;
    ErrorOr<void> set_patterns(ReadonlySpan<String>);

private:
    ContentFilter();
    ~ContentFilter();

    struct Pattern {
        String text;
    };
    Vector<Pattern> m_patterns;
};

}
