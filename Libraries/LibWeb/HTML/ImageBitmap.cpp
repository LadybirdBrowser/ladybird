/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/ImageBitmapPrototype.h>
#include <LibWeb/HTML/ImageBitmap.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ImageBitmap);

GC::Ref<ImageBitmap> ImageBitmap::create(JS::Realm& realm)
{
    return realm.create<ImageBitmap>(realm);
}

ImageBitmap::ImageBitmap(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void ImageBitmap::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ImageBitmap);
    Base::initialize(realm);
}

void ImageBitmap::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

WebIDL::ExceptionOr<void> ImageBitmap::serialization_steps(HTML::TransferDataEncoder&, bool, HTML::SerializationMemory&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) ImageBitmap::serialization_steps(HTML::TransferDataEncoder&, bool, HTML::SerializationMemory&)");
    return {};
}

WebIDL::ExceptionOr<void> ImageBitmap::deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) ImageBitmap::deserialization_steps(ReadonlySpan<u32> const&, size_t&, HTML::DeserializationMemory&)");
    return {};
}

WebIDL::ExceptionOr<void> ImageBitmap::transfer_steps(HTML::TransferDataEncoder&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) ImageBitmap::transfer_steps(HTML::TransferDataEncoder&)");
    return {};
}

WebIDL::ExceptionOr<void> ImageBitmap::transfer_receiving_steps(HTML::TransferDataDecoder&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) ImageBitmap::transfer_receiving_steps(HTML::TransferDataDecoder&)");
    return {};
}

HTML::TransferType ImageBitmap::primary_interface() const
{
    // FIXME: Implement this
    dbgln("(STUBBED) ImageBitmap::primary_interface()");
    return TransferType::Unknown;
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
    m_width = m_bitmap->width();
    m_height = m_bitmap->height();
}

Gfx::Bitmap* ImageBitmap::bitmap() const
{
    return m_bitmap.ptr();
}

}
