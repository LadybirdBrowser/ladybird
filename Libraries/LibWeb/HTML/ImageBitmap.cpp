/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#if defined(AK_OS_LINUX)
#    include <fcntl.h>
#endif

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ImageBitmap);

static constexpr size_t shared_pixels_alignment = 64;

[[nodiscard]] static WebIDL::ExceptionOr<void> validate_bitmap_layout(Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height)
{
    if (!Gfx::is_valid_bitmap_format(to_underlying(format)) || !Gfx::is_valid_alpha_type(to_underlying(alpha_type)))
        return WebIDL::DataCloneError::create("Invalid ImageBitmap pixel format"_utf16);
    Gfx::IntSize const size { width, height };
    if (size.is_empty() || Gfx::Bitmap::size_would_overflow(format, size))
        return WebIDL::DataCloneError::create("Invalid ImageBitmap dimensions"_utf16);
    return {};
}

[[nodiscard]] static WebIDL::ExceptionOr<NonnullRefPtr<Gfx::Bitmap>> create_bitmap_from_bitmap_data(JS::Realm& realm, Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height, size_t const pitch, ByteBuffer data)
{
    TRY(validate_bitmap_layout(format, alpha_type, width, height));
    auto required_size = Checked<size_t> { pitch };
    required_size *= static_cast<size_t>(height);
    if (pitch < Gfx::Bitmap::minimum_pitch(width, format) || required_size.has_overflow() || required_size.value() > data.size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap pixel buffer"_utf16);

    auto bitmap_data = TRY_OR_THROW_OOM(realm.vm(), try_make<ByteBuffer>(move(data)));
    auto* pixels = bitmap_data->data();
    return TRY_OR_THROW_OOM(realm.vm(), Gfx::Bitmap::create_wrapper(format, alpha_type, Gfx::IntSize(width, height), pitch, pixels, [bitmap_data = move(bitmap_data)] { }));
}

template<typename T, typename Encoder>
static WebIDL::ExceptionOr<void> encode_bitmap_value(Encoder& encoder, T const& value)
{
    if constexpr (IsSame<Encoder, HTML::StructuredSerializeWriter>) {
        encoder.encode(value);
    } else {
        if (encoder.encode(value).is_error())
            return WebIDL::DataCloneError::create("Unable to transfer ImageBitmap"_utf16);
    }
    return {};
}

template<typename Encoder>
static WebIDL::ExceptionOr<void> serialize_bitmap(Encoder& encoder, RefPtr<Gfx::Bitmap> const& bitmap)
{
    TRY(encode_bitmap_value(encoder, bitmap != nullptr));
    if (!bitmap)
        return {};

    TRY(encode_bitmap_value(encoder, bitmap->width()));
    TRY(encode_bitmap_value(encoder, bitmap->height()));
    TRY(encode_bitmap_value(encoder, static_cast<u64>(bitmap->pitch())));
    TRY(encode_bitmap_value(encoder, bitmap->format()));
    TRY(encode_bitmap_value(encoder, bitmap->alpha_type()));
    TRY(encode_bitmap_value(encoder, ReadonlyBytes { bitmap->scanline_u8(0), bitmap->data_size() }));
    return {};
}

template<typename Decoder>
[[nodiscard]] static WebIDL::ExceptionOr<RefPtr<Gfx::Bitmap>> deserialize_bitmap(JS::Realm& realm, Decoder& decoder)
{
    auto const has_bitmap = TRY(decode_or_throw_data_clone_error<bool>(realm, decoder));
    if (!has_bitmap)
        return nullptr;
    auto const width = TRY(decode_or_throw_data_clone_error<int>(realm, decoder));
    auto const height = TRY(decode_or_throw_data_clone_error<int>(realm, decoder));
    auto const pitch = TRY(decode_or_throw_data_clone_error<size_t>(realm, decoder));
    auto const format = TRY(decode_or_throw_data_clone_error<Gfx::BitmapFormat>(realm, decoder));
    auto const alpha_type = TRY(decode_or_throw_data_clone_error<Gfx::AlphaType>(realm, decoder));
    auto data = TRY(decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    return TRY(create_bitmap_from_bitmap_data(realm, format, alpha_type, width, height, pitch, move(data)));
}

template<typename T>
[[nodiscard]] static WebIDL::ExceptionOr<T> decode_shared_pixels_value(HTML::TransferDataDecoder& decoder)
{
    auto value = decoder.decode<T>();
    if (value.is_error())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);
    return value.release_value();
}

[[nodiscard]] static WebIDL::ExceptionOr<NonnullRefPtr<Gfx::Bitmap>> bitmap_from_shared_pixels(JS::VM& vm, HTML::TransferDataDecoder& decoder)
{
    auto const buffer_index = TRY(decode_shared_pixels_value<u32>(decoder));
    auto const offset = TRY(decode_shared_pixels_value<u64>(decoder));
    auto const width = TRY(decode_shared_pixels_value<int>(decoder));
    auto const height = TRY(decode_shared_pixels_value<int>(decoder));
    auto const format = TRY(decode_shared_pixels_value<Gfx::BitmapFormat>(decoder));
    auto const alpha_type = TRY(decode_shared_pixels_value<Gfx::AlphaType>(decoder));
    TRY(validate_bitmap_layout(format, alpha_type, width, height));

    auto const* shared_buffers = decoder.shared_buffers();
    if (!shared_buffers || buffer_index >= shared_buffers->size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);
    auto buffer = (*shared_buffers)[buffer_index];

    auto const pitch = Gfx::Bitmap::minimum_pitch(width, format);
    auto const pixel_size = Gfx::Bitmap::size_in_bytes(pitch, height);
    Checked<u64> end = offset;
    end += pixel_size;
    if (offset % alignof(u32) != 0 || end.has_overflow() || end.value() > buffer.size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);

    // NB: Validate resize protection before checking the backing size. A native sender can retain its descriptor
    //     and truncate an unsealed buffer after the size check, faulting any process that accesses its pixels.
#if defined(AK_OS_LINUX)
    auto const seals = fcntl(buffer.fd(), F_GET_SEALS);
    if (seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW))
        return WebIDL::DataCloneError::create("Unsealed ImageBitmap shared pixels"_utf16);
    if (buffer.validate_backing_size().is_error())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap shared pixels"_utf16);
#else
    // NB: Without resize seals, snapshot just this bitmap through an operation that reports truncation instead of
    //     faulting. Copying the entire shared buffer for every bitmap would make batch transfers quadratic.
    auto snapshot = buffer.snapshot(static_cast<size_t>(offset), pixel_size);
    if (snapshot.is_error())
        return WebIDL::DataCloneError::create("Failed to snapshot ImageBitmap shared pixels"_utf16);
    if (offset == 0 && buffer.size() == round_up_to_power_of_two(pixel_size, shared_pixels_alignment))
        return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_with_anonymous_buffer(format, alpha_type, snapshot.release_value(), { width, height }));

    // NB: A batch must not retain a separate snapshot descriptor for every bitmap. Copy the independent pixels into
    //     ordinary bitmap storage, releasing each temporary descriptor before receiving the next bitmap.
    auto bitmap = TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create(format, alpha_type, { width, height }));
    snapshot.value().bytes().copy_to({ bitmap->scanline_u8(0), pixel_size });
    return bitmap;
#endif

    // NB: A buffer that holds just this bitmap backs it directly. Passing the bitmap on, as to WebGL or the compositor,
    //     then shares the memory rather than copying it.
    Gfx::IntSize const size { width, height };
    if (offset == 0 && buffer.size() == round_up_to_power_of_two(Gfx::Bitmap::size_in_bytes(pitch, height), shared_pixels_alignment))
        return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_with_anonymous_buffer(format, alpha_type, move(buffer), size));

    auto* pixels = buffer.data<u8>() + offset;
    return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_wrapper(format, alpha_type, size, pitch, pixels, [buffer = move(buffer)] { }));
}

size_t ImageBitmap::shared_pixels_size(Gfx::Bitmap const& bitmap)
{
    auto size = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(bitmap.width(), bitmap.format()), bitmap.height());
    return round_up_to_power_of_two(size, shared_pixels_alignment);
}

GC::Ref<ImageBitmap> ImageBitmap::create()
{
    return GC::Heap::the().allocate<ImageBitmap>();
}

ImageBitmap::ImageBitmap() = default;

ImageBitmap::~ImageBitmap() = default;

size_t ImageBitmap::external_memory_size() const
{
    auto size = Base::external_memory_size();
    if (m_bitmap)
        size = JS::saturating_add_external_memory_size(size, m_bitmap->data_size());
    return size;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:serialization-steps
WebIDL::ExceptionOr<void> ImageBitmap::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool, HTML::SerializationMemory&)
{
    // FIXME: 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.

    // 2. Set serialized.[[BitmapData]] to a copy of value's bitmap data.
    TRY(serialize_bitmap(serialized, m_bitmap));

    return {};
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:deserialization-steps
WebIDL::ExceptionOr<void> ImageBitmap::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory&)
{
    // 1. Set value's bitmap data to serialized.[[BitmapData]].
    set_bitmap(TRY(deserialize_bitmap(realm, serialized)));

    return {};
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-steps
WebIDL::ExceptionOr<void> ImageBitmap::transfer_steps(JS::Realm&, HTML::TransferDataEncoder& data_holder)
{
    return transfer_steps(data_holder, nullptr);
}

WebIDL::ExceptionOr<void> ImageBitmap::transfer_steps(HTML::TransferDataEncoder& data_holder, SharedPixels* shared_pixels)
{
    // FIXME: 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.

    // 2. Set dataHolder.[[BitmapData]] to value's bitmap data.
    // NB: The pixels go to the shared memory if there is room for them there, and inline otherwise.
    if (shared_pixels && m_bitmap && shared_pixels_size(*m_bitmap) <= shared_pixels->buffer.size() - shared_pixels->used_size) {
        auto const pitch = Gfx::Bitmap::minimum_pitch(m_bitmap->width(), m_bitmap->format());
        auto* pixels = shared_pixels->buffer.data<u8>() + shared_pixels->used_size;
        for (int y = 0; y < m_bitmap->height(); ++y)
            memcpy(pixels + y * pitch, m_bitmap->scanline_u8(y), pitch);

        TRY(encode_bitmap_value(data_holder, true));
        TRY(encode_bitmap_value(data_holder, shared_pixels->buffer_index));
        TRY(encode_bitmap_value(data_holder, static_cast<u64>(shared_pixels->used_size)));
        TRY(encode_bitmap_value(data_holder, m_bitmap->width()));
        TRY(encode_bitmap_value(data_holder, m_bitmap->height()));
        TRY(encode_bitmap_value(data_holder, m_bitmap->format()));
        TRY(encode_bitmap_value(data_holder, m_bitmap->alpha_type()));
        shared_pixels->used_size += shared_pixels_size(*m_bitmap);
    } else {
        TRY(encode_bitmap_value(data_holder, false));
        TRY(serialize_bitmap(data_holder, m_bitmap));
    }

    // 3. Unset value's bitmap data.
    m_bitmap = nullptr;

    return {};
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-receiving-steps
WebIDL::ExceptionOr<void> ImageBitmap::transfer_receiving_steps(JS::Realm& realm, HTML::TransferDataDecoder& data_holder)
{
    // 1. Set value's bitmap data to dataHolder.[[BitmapData]].
    auto const has_shared_pixels = TRY(decode_or_throw_data_clone_error<bool>(realm, data_holder));
    if (has_shared_pixels)
        set_bitmap(TRY(bitmap_from_shared_pixels(realm.vm(), data_holder)));
    else
        set_bitmap(TRY(deserialize_bitmap(realm, data_holder)));

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
