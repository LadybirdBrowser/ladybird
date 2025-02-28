/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/Bindings/ImageDataPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

struct ImageDataSettings {
    Bindings::PredefinedColorSpace color_space;
};

class ImageData final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ImageData, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ImageData);

public:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> create(JS::Realm&, u32 sw, u32 sh, Optional<ImageDataSettings> const& settings = {});
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> create(JS::Realm&, GC::Root<WebIDL::BufferSource> const& data, u32 sw, Optional<u32> sh = {}, Optional<ImageDataSettings> const& settings = {});

    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> construct_impl(JS::Realm&, u32 sw, u32 sh, Optional<ImageDataSettings> const& settings = {});
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> construct_impl(JS::Realm&, GC::Root<WebIDL::BufferSource> const& data, u32 sw, Optional<u32> sh = {}, Optional<ImageDataSettings> const& settings = {});

    virtual ~ImageData() override;

    unsigned width() const;
    unsigned height() const;

    Gfx::Bitmap& bitmap() { return m_bitmap; }
    Gfx::Bitmap const& bitmap() const { return m_bitmap; }

    JS::Uint8ClampedArray* data();
    const JS::Uint8ClampedArray* data() const;

    Bindings::PredefinedColorSpace color_space() const { return m_color_space; }

private:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> initialize(JS::Realm&, u32 rows, u32 pixels_per_row, Optional<ImageDataSettings> const&, GC::Ptr<JS::Uint8ClampedArray> = {}, Optional<Bindings::PredefinedColorSpace> = {});

    ImageData(JS::Realm&, NonnullRefPtr<Gfx::Bitmap>, GC::Ref<JS::Uint8ClampedArray>, Bindings::PredefinedColorSpace);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    NonnullRefPtr<Gfx::Bitmap> m_bitmap;
    Bindings::PredefinedColorSpace m_color_space;
    GC::Ref<JS::Uint8ClampedArray> m_data;
};

}
