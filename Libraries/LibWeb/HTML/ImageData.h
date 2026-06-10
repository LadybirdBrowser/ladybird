/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/ImageData.h>
#include <LibWeb/Bindings/PredefinedColorSpace.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class ImageData final
    : public Bindings::Wrappable
    , public Bindings::Serializable {
    WEB_WRAPPABLE(ImageData, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ImageData);

public:
    using Settings = Bindings::ImageDataSettings;

    [[nodiscard]] static GC::Ref<ImageData> create();
    [[nodiscard]] static GC::Ref<ImageData> create(NonnullRefPtr<Gfx::Bitmap>, GC::Ref<JS::Uint8ClampedArray>, PredefinedColorSpace);
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> create(JS::Realm&, u32 sw, u32 sh, Optional<Settings> const& settings = {});
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> create(JS::Realm&, GC::Ref<JS::Uint8ClampedArray> data, u32 sw, Optional<u32> sh = {}, Optional<Settings> const& settings = {});

    virtual ~ImageData() override;

    WebIDL::UnsignedLong width() const;
    WebIDL::UnsignedLong height() const;

    Gfx::Bitmap& bitmap() { return *m_bitmap; }
    Gfx::Bitmap const& bitmap() const { return *m_bitmap; }

    JS::Uint8ClampedArray* data();
    JS::Uint8ClampedArray const* data() const;

    PredefinedColorSpace color_space() const { return m_color_space; }

    virtual WebIDL::ExceptionOr<void> serialization_steps(JS::Realm&, HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

private:
    [[nodiscard]] static WebIDL::ExceptionOr<GC::Ref<ImageData>> initialize(JS::Realm&, u32 rows, u32 pixels_per_row, Optional<Settings> const&, GC::Ptr<JS::Uint8ClampedArray> = {}, Optional<PredefinedColorSpace> = {});

    ImageData();
    ImageData(NonnullRefPtr<Gfx::Bitmap>, GC::Ref<JS::Uint8ClampedArray>, PredefinedColorSpace);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    RefPtr<Gfx::Bitmap> m_bitmap;
    PredefinedColorSpace m_color_space { PredefinedColorSpace::Srgb };
    GC::Ptr<JS::Uint8ClampedArray> m_data;
};

}
