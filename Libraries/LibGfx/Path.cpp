/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibGfx/PathSkia.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <core/SkPath.h>

extern "C" {
void ladybird_gfx_path_destroy(void*);
bool ladybird_gfx_path_equals(void const*, void const*);
void ladybird_gfx_path_append_svg_string(void const*, void (*)(void*, char const*, size_t), void*);
void ladybird_gfx_path_bounding_box(void const*, float*);
void ladybird_gfx_path_serialize(void const*, void (*)(void*, u8 const*, size_t), void*);
float ladybird_gfx_path_length(void const*);
void* ladybird_gfx_path_create_from_ops(u8 const* kinds, float const* values, size_t count);
void* ladybird_gfx_path_create_from_serialized_bytes(u8 const* bytes, size_t count);
void* ladybird_gfx_path_copy_transformed(void const*, float const* affine_values);
bool ladybird_gfx_path_contains(void const*, float, float, int);
}

namespace Gfx {

Path Path::from_serialized_bytes(ReadonlyBytes bytes)
{
    Gfx::Path path;
    path.impl().deserialize_from_bytes(bytes);
    return path;
}

NonnullOwnPtr<Gfx::PathImpl> PathImpl::create()
{
    return PathImplSkia::create();
}

PathImpl::~PathImpl() = default;

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::Path const& path)
{
    return encoder.encode(path.serialize_to_bytes());
}

template<>
ErrorOr<Gfx::Path> decode(Decoder& decoder)
{
    auto path_data = TRY(decoder.decode<Vector<u8>>());
    return Gfx::Path::from_serialized_bytes(path_data);
}

}

extern "C" void ladybird_gfx_path_destroy(void* path)
{
    delete static_cast<Gfx::Path*>(path);
}

extern "C" void ladybird_gfx_path_append_svg_string(void const* path, void (*append)(void*, char const*, size_t), void* context)
{
    auto string = static_cast<Gfx::Path const*>(path)->to_svg_string();
    append(context, reinterpret_cast<char const*>(string.bytes().data()), string.bytes().size());
}

extern "C" void ladybird_gfx_path_bounding_box(void const* path, float* out_x_y_width_height)
{
    auto rect = static_cast<Gfx::Path const*>(path)->bounding_box();
    out_x_y_width_height[0] = rect.x();
    out_x_y_width_height[1] = rect.y();
    out_x_y_width_height[2] = rect.width();
    out_x_y_width_height[3] = rect.height();
}

extern "C" void ladybird_gfx_path_serialize(void const* path, void (*append)(void*, u8 const*, size_t), void* context)
{
    auto bytes = static_cast<Gfx::Path const*>(path)->serialize_to_bytes();
    append(context, bytes.data(), bytes.size());
}

extern "C" float ladybird_gfx_path_length(void const* path)
{
    return static_cast<Gfx::Path const*>(path)->length();
}

enum class PathBuilderOpKind : u8 {
    MoveTo,
    LineTo,
    QuadraticBezierCurveTo,
    CubicBezierCurveTo,
    EllipticalArcTo,
    ArcTo,
    Close,
};

extern "C" void* ladybird_gfx_path_create_from_ops(u8 const* kinds, float const* values, size_t count)
{
    auto* path = new Gfx::Path;
    for (size_t i = 0; i < count; ++i) {
        auto const* v = values + i * 7;
        switch (static_cast<PathBuilderOpKind>(kinds[i])) {
        case PathBuilderOpKind::MoveTo:
            path->move_to({ v[0], v[1] });
            break;
        case PathBuilderOpKind::LineTo:
            path->line_to({ v[0], v[1] });
            break;
        case PathBuilderOpKind::QuadraticBezierCurveTo:
            path->quadratic_bezier_curve_to({ v[0], v[1] }, { v[2], v[3] });
            break;
        case PathBuilderOpKind::CubicBezierCurveTo:
            path->cubic_bezier_curve_to({ v[0], v[1] }, { v[2], v[3] }, { v[4], v[5] });
            break;
        case PathBuilderOpKind::EllipticalArcTo:
            path->elliptical_arc_to({ v[0], v[1] }, { v[2], v[3] }, v[4], v[5] != 0, v[6] != 0);
            break;
        case PathBuilderOpKind::ArcTo:
            path->arc_to({ v[0], v[1] }, v[2], v[3] != 0, v[4] != 0);
            break;
        case PathBuilderOpKind::Close:
            path->close();
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
    return path;
}

extern "C" void* ladybird_gfx_path_create_from_serialized_bytes(u8 const* bytes, size_t count)
{
    auto path_bytes_at_an_aligned_address = MUST(ByteBuffer::copy(bytes, count));
    return new Gfx::Path(Gfx::Path::from_serialized_bytes(path_bytes_at_an_aligned_address));
}

extern "C" void* ladybird_gfx_path_copy_transformed(void const* path, float const* affine_values)
{
    Gfx::AffineTransform transform { affine_values[0], affine_values[1], affine_values[2], affine_values[3], affine_values[4], affine_values[5] };
    return new Gfx::Path(static_cast<Gfx::Path const*>(path)->copy_transformed(transform));
}

extern "C" bool ladybird_gfx_path_contains(void const* path, float x, float y, int winding_rule)
{
    return static_cast<Gfx::Path const*>(path)->contains({ x, y }, static_cast<Gfx::WindingRule>(winding_rule));
}

extern "C" bool ladybird_gfx_path_equals(void const* a, void const* b)
{
    auto const& path_a = *static_cast<Gfx::Path const*>(a);
    auto const& path_b = *static_cast<Gfx::Path const*>(b);
    return static_cast<Gfx::PathImplSkia const&>(path_a.impl()).sk_path() == static_cast<Gfx::PathImplSkia const&>(path_b.impl()).sk_path();
}
