/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGfx/BitmapExportResult.h>
#include <LibGfx/Color.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>

class SkImage;

namespace Gfx {

struct ImmutableBitmapImpl;

#define ENUMERATE_EXPORT_FORMATS(X) \
    X(Gray8)                        \
    X(Alpha8)                       \
    X(RGB565)                       \
    X(RGBA5551)                     \
    X(RGBA4444)                     \
    X(RGB888)                       \
    X(RGBA8888)

enum class ExportFormat : u8 {
#define ENUMERATE_EXPORT_FORMAT(format) format,
    ENUMERATE_EXPORT_FORMATS(ENUMERATE_EXPORT_FORMAT)
#undef ENUMERATE_EXPORT_FORMAT
};

[[nodiscard]] StringView export_format_name(ExportFormat);

struct ExportFlags {
    enum : u8 {
        PremultiplyAlpha = 1 << 0,
        FlipY = 1 << 1,
    };
};

class ImmutableBitmap final : public AtomicRefCounted<ImmutableBitmap> {
public:
    static NonnullRefPtr<ImmutableBitmap> create(NonnullRefPtr<Bitmap> bitmap, ColorSpace color_space = {});
    static NonnullRefPtr<ImmutableBitmap> create(NonnullRefPtr<Bitmap> bitmap, AlphaType, ColorSpace color_space = {});
    static NonnullRefPtr<ImmutableBitmap> create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface>);

    ~ImmutableBitmap();

    int width() const;
    int height() const;
    IntRect rect() const;
    IntSize size() const;

    AlphaType alpha_type() const;

    SkImage const* sk_image() const;
    [[nodiscard]] ErrorOr<BitmapExportResult> export_to_byte_buffer(ExportFormat format, int flags, Optional<int> target_width, Optional<int> target_height) const;

    Color get_pixel(int x, int y) const;

    RefPtr<Bitmap const> bitmap() const;

private:
    NonnullOwnPtr<ImmutableBitmapImpl> m_impl;

    explicit ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> bitmap);
};

}
