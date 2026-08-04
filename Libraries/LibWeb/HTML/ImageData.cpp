/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ImageData);

[[nodiscard]] static ErrorOr<void> validate_uint8_clamped_array_can_back_bitmap(u32 const width, u32 const height, JS::Uint8ClampedArray const& data)
{
    auto data_record = JS::make_typed_array_with_buffer_witness_record(data, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(data_record))
        return Error::from_errno(EINVAL);

    Checked<size_t> expected_byte_length = width;
    expected_byte_length *= height;
    expected_byte_length *= sizeof(u32);
    if (expected_byte_length.has_overflow())
        return Error::from_errno(EINVAL);

    if (JS::typed_array_byte_length(data_record) < expected_byte_length.value())
        return Error::from_errno(EINVAL);

    return {};
}

[[nodiscard]] static ErrorOr<NonnullRefPtr<Gfx::Bitmap>> create_bitmap_backed_by_uint8_clamped_array(u32 const width, u32 const height, JS::Uint8ClampedArray& data)
{
    TRY(validate_uint8_clamped_array_can_back_bitmap(width, height, data));
    return Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, Gfx::IntSize(width, height), width * sizeof(u32), data.viewed_array_buffer()->data_at(data.byte_offset()));
}

GC::Ref<ImageData> ImageData::create()
{
    return GC::Heap::the().allocate<ImageData>();
}

GC::Ref<ImageData> ImageData::create(NonnullRefPtr<Gfx::Bitmap> bitmap, GC::Ref<JS::Uint8ClampedArray> data, PredefinedColorSpace color_space)
{
    return GC::Heap::the().allocate<ImageData>(move(bitmap), data, color_space);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-imagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> ImageData::create(JS::Realm& realm, u32 sw, u32 sh, Optional<Settings> const& settings)
{
    // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
    if (sw == 0 || sh == 0)
        return WebIDL::IndexSizeError::create("The source width and height must be greater than zero."_utf16);

    // 2. Initialize this given sw, sh, and settings set to settings.
    // 3. Initialize the image data of this to transparent black.
    return initialize(realm, sh, sw, settings);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-imagedata-with-data
WebIDL::ExceptionOr<GC::Ref<ImageData>> ImageData::create(JS::Realm& realm, GC::Ref<JS::Uint8ClampedArray> uint8_clamped_array_data, u32 sw, Optional<u32> sh, Optional<Settings> const& settings)
{
    auto data_record = JS::make_typed_array_with_buffer_witness_record(*uint8_clamped_array_data, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(data_record))
        return WebIDL::InvalidStateError::create(realm, "Source data must not be out-of-bounds."_utf16);

    // 1. Let length be the number of bytes in data.
    auto length = JS::typed_array_length(data_record);

    // 2. If length is not a nonzero integral multiple of four, then throw an "InvalidStateError" DOMException.
    if (length == 0 || length % 4 != 0)
        return WebIDL::InvalidStateError::create("Source data must have a non-sero length that is a multiple of four."_utf16);

    // 3. Let length be length divided by four.
    length = length / 4;

    // 4. If length is not an integral multiple of sw, then throw an "IndexSizeError" DOMException.
    // NOTE: At this step, the length is guaranteed to be greater than zero (otherwise the second step above would have aborted the steps),
    //       so if sw is zero, this step will throw the exception and return.
    if (sw == 0 || length % sw != 0)
        return WebIDL::IndexSizeError::create("Source width must be a multiple of source data's length."_utf16);

    // 5. Let height be length divided by sw.
    auto height = length / sw;

    // 6. If sh was given and its value is not equal to height, then throw an "IndexSizeError" DOMException.
    if (sh.has_value() && sh.value() != height)
        return WebIDL::IndexSizeError::create("Source height must be equal to the calculated height of the data."_utf16);

    // 7. Initialize this given sw, sh, settings set to settings, and source set to data.
    // FIXME: This seems to be a spec issue, sh is an optional but height always have a value.
    return initialize(realm, height, sw, settings, *uint8_clamped_array_data);
}

// https://html.spec.whatwg.org/multipage/canvas.html#initialize-an-imagedata-object
WebIDL::ExceptionOr<GC::Ref<ImageData>> ImageData::initialize(JS::Realm& realm, u32 rows, u32 pixels_per_row, Optional<Settings> const& settings, GC::Ptr<JS::Uint8ClampedArray> source, Optional<PredefinedColorSpace> default_color_space)
{
    auto data = TRY([&]() -> WebIDL::ExceptionOr<GC::Ref<JS::Uint8ClampedArray>> {
        // 1. If source was given, then initialize the data attribute of imageData to source.
        if (source) {
            return GC::Ref<JS::Uint8ClampedArray> { *source };
        }

        Checked<u32> size = rows;
        size *= pixels_per_row;
        size *= sizeof(u32);
        if (size.has_overflow())
            return WebIDL::IndexSizeError::create("The specified image size could not created"_utf16);

        // 2. Otherwise (source was not given), initialize the data attribute of imageData to a new Uint8ClampedArray object.
        //    The Uint8ClampedArray object must use a new Canvas Pixel ArrayBuffer for its storage, and must have a zero start
        //    offset and a length equal to the length of its storage, in bytes. The Canvas Pixel ArrayBuffer must have the
        //    correct size to store rows × pixelsPerRow pixels.
        // 3. If the Canvas Pixel ArrayBuffer cannot be allocated, then rethrow the RangeError thrown by JavaScript, and return.
        return TRY(JS::Uint8ClampedArray::create(realm, sizeof(u32) * rows * pixels_per_row));
    }());

    // AD-HOC: Validate that the bitmap can be backed by the Uint8ClampedArray.
    if (validate_uint8_clamped_array_can_back_bitmap(pixels_per_row, rows, *data).is_error())
        return WebIDL::InvalidStateError::create(realm, "Source data must not be detached or out-of-bounds."_utf16);

    // 4. Initialize the width attribute of imageData to pixelsPerRow.
    // 5. Initialize the height attribute of imageData to rows.

    // 6. If settings was given and settings["colorSpace"] exists, then initialize the colorSpace attribute of imageData to settings["colorSpace"].
    PredefinedColorSpace color_space {};
    if (settings.has_value() && settings->color_space.has_value())
        color_space = *settings->color_space;
    // 7. Otherwise, if defaultColorSpace was given, then initialize the colorSpace attribute of imageData to defaultColorSpace.
    else if (default_color_space.has_value())
        color_space = *default_color_space;
    // 8. Otherwise, initialize the colorSpace attribute of imageData to "srgb".
    else
        color_space = PredefinedColorSpace::Srgb;

    auto bitmap = MUST(create_bitmap_backed_by_uint8_clamped_array(pixels_per_row, rows, *data));
    return ImageData::create(move(bitmap), data, color_space);
}

ImageData::ImageData()
{
}

ImageData::ImageData(NonnullRefPtr<Gfx::Bitmap> bitmap, GC::Ref<JS::Uint8ClampedArray> data, PredefinedColorSpace color_space)
    : m_bitmap(move(bitmap))
    , m_color_space(color_space)
    , m_data(move(data))
{
}

ImageData::~ImageData() = default;

void ImageData::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
}

WebIDL::UnsignedLong ImageData::width() const
{
    return m_bitmap ? m_bitmap->width() : 0;
}

WebIDL::UnsignedLong ImageData::height() const
{
    return m_bitmap ? m_bitmap->height() : 0;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> ImageData::bitmap()
{
    if (!m_bitmap)
        return Error::from_errno(EINVAL);
    return *m_bitmap;
}

JS::Uint8ClampedArray* ImageData::data()
{
    return m_data;
}

JS::Uint8ClampedArray const* ImageData::data() const
{
    return m_data;
}

// https://html.spec.whatwg.org/multipage/canvas.html#pixel-manipulation:serialization-steps
WebIDL::ExceptionOr<void> ImageData::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    // 1. Set serialized.[[Data]] to the sub-serialization of the value of value's data attribute.
    if (m_data)
        TRY(structured_serialize_internal(m_data->vm(), serialized, JS::Value(m_data.ptr()), for_storage, memory));
    else
        serialized.encode(to_underlying(ValueTag::NullPrimitive));

    // 2. Set serialized.[[Width]] to the value of value's width attribute.
    serialized.encode(width());

    // 3. Set serialized.[[Height]] to the value of value's height attribute.
    serialized.encode(height());

    // 4. Set serialized.[[ColorSpace]] to the value of value's colorSpace attribute.
    serialized.encode(m_color_space);

    // FIXME:: 5. Set serialized.[[PixelFormat]] to the value of value's pixelFormat attribute.

    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#pixel-manipulation:deserialization-steps
WebIDL::ExceptionOr<void> ImageData::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory& memory)
{
    auto data_value = TRY(structured_deserialize_internal(realm.vm(), serialized, realm, memory, CheckFullyConsumed::No));
    if (!data_value.is_object() || !is<JS::Uint8ClampedArray>(data_value.as_object()))
        return WebIDL::DataCloneError::create("ImageData data must be a Uint8ClampedArray"_utf16);
    m_data = &as<JS::Uint8ClampedArray>(data_value.as_object());

    // 2. Initialize value's width attribute to serialized.[[Width]].
    auto width = TRY(HTML::decode_or_throw_data_clone_error<u32>(realm, serialized));

    // 3. Initialize value's height attribute to serialized.[[Height]].
    auto height = TRY(HTML::decode_or_throw_data_clone_error<u32>(realm, serialized));

    // 4. Initialize value's colorSpace attribute to serialized.[[ColorSpace]].
    m_color_space = TRY(HTML::decode_or_throw_data_clone_error<PredefinedColorSpace>(realm, serialized));

    // FIXME: 5. Initialize value's pixelFormat attribute to serialized.[[PixelFormat]].

    // AD-HOC: Validate that the bitmap can be backed by the Uint8ClampedArray.
    if (validate_uint8_clamped_array_can_back_bitmap(width, height, *m_data).is_error())
        return WebIDL::InvalidStateError::create(realm, "Image data must not be detached or out-of-bounds."_utf16);

    m_bitmap = MUST(create_bitmap_backed_by_uint8_clamped_array(width, height, *m_data));

    return {};
}

}
