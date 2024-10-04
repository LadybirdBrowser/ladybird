/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ImageDataPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ImageData);

// https://html.spec.whatwg.org/multipage/canvas.html#dom-imagedata
WebIDL::ExceptionOr<JS::NonnullGCPtr<ImageData>> ImageData::create(JS::Realm& realm, u32 sw, u32 sh, Optional<ImageDataSettings> const&)
{
    auto& vm = realm.vm();

    // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
    if (sw == 0 || sh == 0)
        return WebIDL::IndexSizeError::create(realm, "The source width and height must be greater than zero."_fly_string);

    // 2. Initialize this given sw, sh, and settings set to settings.
    // 3. Initialize the image data of this to transparent black.
    auto data = TRY(JS::Uint8ClampedArray::create(realm, sw * sh * 4));
    auto bitmap = TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, Gfx::IntSize(sw, sh), sw * sizeof(u32), data->data().data()));

    return realm.heap().allocate<ImageData>(realm, realm, bitmap, data);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<ImageData>> ImageData::construct_impl(JS::Realm& realm, u32 sw, u32 sh, Optional<ImageDataSettings> const& settings)
{
    return ImageData::create(realm, sw, sh, settings);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-imagedata-with-data
WebIDL::ExceptionOr<JS::NonnullGCPtr<ImageData>> ImageData::create(JS::Realm& realm, JS::Handle<WebIDL::BufferSource> const& data, u32 sw, Optional<u32> sh, Optional<ImageDataSettings> const&)
{
    auto& vm = realm.vm();

    if (!is<JS::Uint8ClampedArray>(*data->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Uint8ClampedArray");

    auto& uint8_clamped_array_data = static_cast<JS::Uint8ClampedArray&>(*data->raw_object());

    // 1. Let length be the number of bytes in data.
    auto length = uint8_clamped_array_data.byte_length().length();

    // 2. If length is not a nonzero integral multiple of four, then throw an "InvalidStateError" DOMException.
    if (length == 0 || length % 4 != 0)
        return WebIDL::InvalidStateError::create(realm, "Source data must have a non-sero length that is a multiple of four."_fly_string);

    // 3. Let length be length divided by four.
    length = length / 4;

    // 4. If length is not an integral multiple of sw, then throw an "IndexSizeError" DOMException.
    // NOTE: At this step, the length is guaranteed to be greater than zero (otherwise the second step above would have aborted the steps),
    //       so if sw is zero, this step will throw the exception and return.
    if (sw == 0 || length % sw != 0)
        return WebIDL::IndexSizeError::create(realm, "Source width must be a multiple of source data's length."_fly_string);

    // 5. Let height be length divided by sw.
    auto height = length / sw;

    // 6. If sh was given and its value is not equal to height, then throw an "IndexSizeError" DOMException.
    if (sh.has_value() && sh.value() != height)
        return WebIDL::IndexSizeError::create(realm, "Source height must be equal to the calculated height of the data."_fly_string);

    // 7. Initialize this given sw, sh, settings set to settings, and source set to data.
    auto bitmap = TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, Gfx::IntSize(sw, height), sw * sizeof(u32), uint8_clamped_array_data.data().data()));

    return realm.heap().allocate<ImageData>(realm, realm, bitmap, uint8_clamped_array_data);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<ImageData>> ImageData::construct_impl(JS::Realm& realm, JS::Handle<WebIDL::BufferSource> const& data, u32 sw, Optional<u32> sh, Optional<ImageDataSettings> const& settings)
{
    return ImageData::create(realm, data, sw, move(sh), settings);
}

ImageData::ImageData(JS::Realm& realm, NonnullRefPtr<Gfx::Bitmap> bitmap, JS::NonnullGCPtr<JS::Uint8ClampedArray> data)
    : PlatformObject(realm)
    , m_bitmap(move(bitmap))
    , m_data(move(data))
{
}

ImageData::~ImageData() = default;

void ImageData::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ImageData);
}

void ImageData::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
}

unsigned ImageData::width() const
{
    return m_bitmap->width();
}

unsigned ImageData::height() const
{
    return m_bitmap->height();
}

JS::Uint8ClampedArray* ImageData::data()
{
    return m_data;
}

const JS::Uint8ClampedArray* ImageData::data() const
{
    return m_data;
}

}
