/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGfx/Forward.h>

namespace Web::SVG {

class AttributeParser;

class Path {
public:
    Path();
    Path(Path const&);
    Path(Path&&);
    ~Path();

    Path& operator=(Path const&);
    Path& operator=(Path&&);

    [[nodiscard]] Gfx::Path to_gfx_path() const;
    String serialize() const;

    bool operator==(Path const&) const;

private:
    friend class AttributeParser;

    explicit Path(void* rust_path)
        : m_rust_path(rust_path)
    {
    }

    void* m_rust_path { nullptr };
};

}
