/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Utf16String.h>
#include <LibWeb/CSS/RustQueryHandle.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

namespace Parser {

class RustQueryParser;

}

// https://www.w3.org/TR/css-conditional-3/#at-supports
class WEB_API Supports final : public RefCounted<Supports> {
    friend class Parser::RustQueryParser;

public:
    static NonnullRefPtr<Supports> create(RustQueryHandle handle)
    {
        return adopt_ref(*new Supports(move(handle)));
    }

    bool matches() const;
    Utf16String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    explicit Supports(RustQueryHandle handle)
        : m_rust_query_handle(move(handle))
    {
    }

    RustQueryHandle m_rust_query_handle;
};

}
