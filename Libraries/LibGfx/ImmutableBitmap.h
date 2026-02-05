/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
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
#include <LibGfx/YUVData.h>

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
    static ErrorOr<NonnullRefPtr<ImmutableBitmap>> create_from_yuv(NonnullOwnPtr<YUVData>);

    ~ImmutableBitmap();

    bool is_yuv_backed() const;
    bool ensure_sk_image(SkiaBackendContext&) const;

    int width() const;
    int height() const;
    IntRect rect() const;
    IntSize size() const;

    AlphaType alpha_type() const;

    SkImage const* sk_image() const;
    [[nodiscard]] ErrorOr<BitmapExportResult> export_to_byte_buffer(ExportFormat format, int flags, Optional<int> target_width, Optional<int> target_height) const;

    Color get_pixel(int x, int y) const;

    // Returns nullptr for YUV-backed bitmaps
    RefPtr<Bitmap const> bitmap() const;

private:
    friend class YUVData;

    mutable NonnullOwnPtr<ImmutableBitmapImpl> m_impl;

    explicit ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl> bitmap);

    void lock_context();
    void unlock_context();
};

}
