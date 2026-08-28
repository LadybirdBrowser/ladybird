/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibGfx/Path.h>
#include <LibWeb/SVG/ParserRustFFI.h>
#include <LibWeb/SVG/Path.h>

namespace Web::SVG {

static RustFFI::FfiSvgInput ffi_svg_input(Utf16View input)
{
    return {
        .ascii = input.has_ascii_storage() ? reinterpret_cast<u8 const*>(input.ascii_span().data()) : nullptr,
        .utf16 = input.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(input.utf16_span().data()),
        .length = input.length_in_code_units(),
    };
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

Gfx::Path Path::to_gfx_path() const
{
    auto* path = static_cast<Gfx::Path*>(RustFFI::rust_svg_path_to_gfx_path(m_rust_path));
    auto result = move(*path);
    delete path;
    return result;
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
