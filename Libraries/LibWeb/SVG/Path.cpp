/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/SVG/ParserRustFFI.h>
#include <LibWeb/SVG/Path.h>

namespace Web::SVG {

static RustFFI::FfiUtf16View ffi_svg_input(Utf16View input)
{
    return {
        .ascii = input.has_ascii_storage() ? reinterpret_cast<u8 const*>(input.ascii_span().data()) : nullptr,
        .utf16 = input.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(input.utf16_span().data()),
        .length = input.length_in_code_units(),
    };
}

void append_path_vertices(void* context, Layout::RustFFI::FfiPathVertex const* vertices, size_t count)
{
    auto& path_vertices = *static_cast<Vector<PathVertex>*>(context);
    path_vertices.ensure_capacity(path_vertices.size() + count);
    for (auto const& vertex : ReadonlySpan<Layout::RustFFI::FfiPathVertex> { vertices, count })
        path_vertices.unchecked_append({ { vertex.point.x, vertex.point.y }, vertex.distance, { vertex.direction.x, vertex.direction.y } });
}

Path parse_path_data(Utf16View input)
{
    auto* path = RustFFI::rust_parse_svg_path_data(ffi_svg_input(input));
    return path ? Path { path } : Path {};
}

Path::Path()
    : m_rust_path(RustFFI::rust_parse_svg_path_data({}))
{
    VERIFY(m_rust_path);
}

Path::Path(Path const& other)
    : m_rust_path(RustFFI::rust_svg_path_clone(other.m_rust_path))
{
}

Path::Path(Path&& other)
    : Path()
{
    swap(m_rust_path, other.m_rust_path);
}

Path::~Path()
{
    if (m_rust_path)
        RustFFI::rust_svg_path_destroy(m_rust_path);
}

Path& Path::operator=(Path const& other)
{
    if (this == &other)
        return *this;
    auto* new_path = RustFFI::rust_svg_path_clone(other.m_rust_path);
    if (m_rust_path)
        RustFFI::rust_svg_path_destroy(m_rust_path);
    m_rust_path = new_path;
    return *this;
}

Path& Path::operator=(Path&& other)
{
    if (this == &other)
        return *this;
    Path moved_path { move(other) };
    swap(m_rust_path, moved_path.m_rust_path);
    return *this;
}

Vector<PathVertex> Path::vertices() const
{
    Vector<PathVertex> vertices;
    Layout::RustFFI::svg_path_data_vertices(m_rust_path, &vertices, append_path_vertices);
    return vertices;
}

String Path::serialize() const
{
    StringBuilder builder;
    auto append = [](void* context, u8 const* bytes, size_t length) {
        static_cast<StringBuilder*>(context)->append(StringView { reinterpret_cast<char const*>(bytes), length });
    };
    RustFFI::rust_svg_path_serialize(m_rust_path, &builder, append);
    return builder.to_string_without_validation();
}

bool Path::operator==(Path const& other) const
{
    return RustFFI::rust_svg_path_equals(m_rust_path, other.m_rust_path);
}

}
