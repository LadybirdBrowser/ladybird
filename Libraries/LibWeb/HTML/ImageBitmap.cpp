/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/ImageBitmapPrototype.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ImageBitmap);

[[nodiscard]] static auto create_bitmap_from_bitmap_data(Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, u32 const width, u32 const height, u32 const pitch, ByteBuffer data)
{
    return Gfx::Bitmap::create_wrapper(format, alpha_type, Gfx::IntSize(width, height), pitch, data.data());
}

static void serialize_bitmap(HTML::TransferDataEncoder& encoder, RefPtr<Gfx::Bitmap> const& bitmap)
{
    if (!bitmap) {
        encoder.encode(0);
        return;
    }

    encoder.encode(bitmap->width());
    encoder.encode(bitmap->height());
    encoder.encode(bitmap->pitch());
    encoder.encode(bitmap->format());
    encoder.encode(bitmap->alpha_type());
    encoder.encode(ReadonlyBytes { bitmap->scanline_u8(0), bitmap->data_size() });
}

[[nodiscard]] static WebIDL::ExceptionOr<RefPtr<Gfx::Bitmap>> deserialize_bitmap(JS::Realm& realm, HTML::TransferDataDecoder& decoder)
{
    auto const width = decoder.decode<int>();
    if (width == 0)
        return nullptr;
    auto const height = decoder.decode<int>();
    auto const pitch = decoder.decode<size_t>();
    auto const format = decoder.decode<Gfx::BitmapFormat>();
    auto const alpha_type = decoder.decode<Gfx::AlphaType>();
    auto const data = TRY(decoder.decode_buffer(realm));
    return TRY_OR_THROW_OOM(realm.vm(), create_bitmap_from_bitmap_data(format, alpha_type, width, height, pitch, data));
}

GC::Ref<ImageBitmap> ImageBitmap::create(JS::Realm& realm)
{
    return realm.create<ImageBitmap>(realm);
}

ImageBitmap::ImageBitmap(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

ImageBitmap::~ImageBitmap() = default;

void ImageBitmap::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ImageBitmap);
    Base::initialize(realm);
}

void ImageBitmap::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:serialization-steps
WebIDL::ExceptionOr<void> ImageBitmap::serialization_steps(HTML::TransferDataEncoder& serialized, bool, HTML::SerializationMemory&)
{
    // FIXME: 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.

    // 2. Set serialized.[[BitmapData]] to a copy of value's bitmap data.
    serialize_bitmap(serialized, m_bitmap);

    return {};
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:deserialization-steps
WebIDL::ExceptionOr<void> ImageBitmap::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory&)
{
    // 1. Set value's bitmap data to serialized.[[BitmapData]].
    set_bitmap(TRY(deserialize_bitmap(this->realm(), serialized)));

    return {};
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-steps
WebIDL::ExceptionOr<void> ImageBitmap::transfer_steps(HTML::TransferDataEncoder& data_holder)
{
    // FIXME: 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.

    // 2. Set dataHolder.[[BitmapData]] to value's bitmap data.
    serialize_bitmap(data_holder, m_bitmap);

    // 3. Unset value's bitmap data.
    m_bitmap = nullptr;

    return {};
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-receiving-steps
WebIDL::ExceptionOr<void> ImageBitmap::transfer_receiving_steps(HTML::TransferDataDecoder& data_holder)
{
    // 1. Set value's bitmap data to dataHolder.[[BitmapData]].
    set_bitmap(TRY(deserialize_bitmap(this->realm(), data_holder)));

    return {};
}

HTML::TransferType ImageBitmap::primary_interface() const
{
    return TransferType::ImageBitmap;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-width
WebIDL::UnsignedLong ImageBitmap::width() const
{
    // 1. If this's [[Detached]] internal slot's value is true, then return 0.
    if (is_detached())
        return 0;
    // 2. Return this's width, in CSS pixels.
    return m_width;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-height
WebIDL::UnsignedLong ImageBitmap::height() const
{
    // 1. If this's [[Detached]] internal slot's value is true, then return 0.
    if (is_detached())
        return 0;
    // 2. Return this's height, in CSS pixels.
    return m_height;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-close
void ImageBitmap::close()
{
    // 1. Set this's [[Detached]] internal slot value to true.
    set_detached(true);

    // 2. Unset this's bitmap data.
    m_bitmap = nullptr;
}

void ImageBitmap::set_bitmap(RefPtr<Gfx::Bitmap> bitmap)
{
    m_bitmap = move(bitmap);
    m_width = m_bitmap ? m_bitmap->width() : 0;
    m_height = m_bitmap ? m_bitmap->height() : 0;
}

Gfx::Bitmap* ImageBitmap::bitmap() const
{
    return m_bitmap.ptr();
}

}
