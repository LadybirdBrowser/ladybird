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
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibGfx/Vector2.h>

namespace Web::Layout::RustFFI {

struct FfiPathVertex;

}

namespace Web::SVG {

// A vertex of polylines through a path, at `distance` along it, where the path heads in `direction`.
struct PathVertex {
    Gfx::FloatPoint point;
    float distance { 0 };
    Gfx::FloatVector2 direction;
};

// Appends the `count` vertices of a path that Rust measured to the Vector<PathVertex> `context` points to.
void append_path_vertices(void* context, Layout::RustFFI::FfiPathVertex const*, size_t count);

class Path;
Path parse_path_data(Utf16View);

class Path {
public:
    Path();
    Path(Path const&);
    Path(Path&&);
    ~Path();

    Path& operator=(Path const&);
    Path& operator=(Path&&);

    // The vertices of polylines through the path, as close to its curves as its length is measured.
    Vector<PathVertex> vertices() const;
    String serialize() const;

    bool operator==(Path const&) const;

private:
    friend Path parse_path_data(Utf16View);

    explicit Path(void* rust_path)
        : m_rust_path(rust_path)
    {
    }

    void* m_rust_path { nullptr };
};

}
