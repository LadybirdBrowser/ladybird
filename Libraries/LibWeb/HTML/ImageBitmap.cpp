/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#if defined(AK_OS_LINUX)
#    include <fcntl.h>
#endif

namespace Web::HTML {

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

[[nodiscard]] static WebIDL::ExceptionOr<NonnullRefPtr<Gfx::Bitmap>> create_bitmap_from_bitmap_data(JS::VM& vm, Gfx::BitmapFormat const format, Gfx::AlphaType const alpha_type, int const width, int const height, size_t const pitch, ByteBuffer data)
{
    TRY(validate_bitmap_layout(format, alpha_type, width, height));
    auto required_size = Checked<size_t> { pitch };
    required_size *= static_cast<size_t>(height);
    if (pitch < Gfx::Bitmap::minimum_pitch(width, format) || required_size.has_overflow() || required_size.value() > data.size())
        return WebIDL::DataCloneError::create("Invalid ImageBitmap pixel buffer"_utf16);

    auto bitmap_data = TRY_OR_THROW_OOM(vm, try_make<ByteBuffer>(move(data)));
    auto* pixels = bitmap_data->data();
    return TRY_OR_THROW_OOM(vm, Gfx::Bitmap::create_wrapper(format, alpha_type, Gfx::IntSize(width, height), pitch, pixels, [bitmap_data = move(bitmap_data)] { }));
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

enum class SerializedColorSpaceKind : u8 {
    SRGB,
    CICP,
    ICCProfile,
};

template<typename Encoder>
static WebIDL::ExceptionOr<void> serialize_color_space(Encoder& encoder, Gfx::ColorSpace const& color_space)
{
    return color_space.data().visit(
        [&](Empty) -> WebIDL::ExceptionOr<void> {
            return encode_bitmap_value(encoder, to_underlying(SerializedColorSpaceKind::SRGB));
        },
        [&](Gfx::ColorSpace::CICP const& cicp) -> WebIDL::ExceptionOr<void> {
            TRY(encode_bitmap_value(encoder, to_underlying(SerializedColorSpaceKind::CICP)));
            TRY(encode_bitmap_value(encoder, to_underlying(cicp.color_primaries)));
            TRY(encode_bitmap_value(encoder, to_underlying(cicp.transfer_characteristics)));
            return {};
        },
        [&](NonnullRefPtr<Gfx::ColorSpace::ICCProfile const> const& icc_profile) -> WebIDL::ExceptionOr<void> {
            TRY(encode_bitmap_value(encoder, to_underlying(SerializedColorSpaceKind::ICCProfile)));
            TRY(encode_bitmap_value(encoder, icc_profile->bytes()));
            return {};
        });
}

template<typename Decoder>
[[nodiscard]] static ErrorOr<Gfx::ColorSpace> deserialize_color_space(Decoder& decoder)
{
    auto kind = TRY(decoder.template decode<u8>());
    switch (static_cast<SerializedColorSpaceKind>(kind)) {
    case SerializedColorSpaceKind::SRGB:
        return Gfx::ColorSpace {};
    case SerializedColorSpaceKind::CICP: {
        auto color_primaries = static_cast<Media::ColorPrimaries>(TRY(decoder.template decode<u8>()));
        auto transfer_characteristics = static_cast<Media::TransferCharacteristics>(TRY(decoder.template decode<u8>()));
        return Gfx::ColorSpace::from_cicp({ color_primaries, transfer_characteristics, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full });
    }
    case SerializedColorSpaceKind::ICCProfile: {
        auto icc_bytes = TRY(decoder.template decode<ByteBuffer>());
        return Gfx::ColorSpace::from_icc_bytes(icc_bytes);
    }
    }
    return Error::from_string_literal("Invalid ImageBitmap color space");
}

// NB: Records of a bitmap in sRGB keep the layout they had before ImageBitmaps kept a color space, so stored records
//     of either kind decode.
enum class SerializedBitmapKind : u8 {
    None,
    SRGB,
    WithColorSpace,
};

template<typename Encoder>
static WebIDL::ExceptionOr<void> serialize_bitmap(Encoder& encoder, Gfx::DecodedImageFrame const* frame)
{
    if (!frame)
        return encode_bitmap_value(encoder, to_underlying(SerializedBitmapKind::None));

    auto const& bitmap = frame->bitmap();
    auto const has_color_space = !frame->color_space().data().has<Empty>();
    TRY(encode_bitmap_value(encoder, to_underlying(has_color_space ? SerializedBitmapKind::WithColorSpace : SerializedBitmapKind::SRGB)));
    TRY(encode_bitmap_value(encoder, bitmap.width()));
    TRY(encode_bitmap_value(encoder, bitmap.height()));
    TRY(encode_bitmap_value(encoder, static_cast<u64>(bitmap.pitch())));
    TRY(encode_bitmap_value(encoder, bitmap.format()));
    TRY(encode_bitmap_value(encoder, bitmap.alpha_type()));
    TRY(encode_bitmap_value(encoder, ReadonlyBytes { bitmap.scanline_u8(0), bitmap.data_size() }));
    if (has_color_space)
        TRY(serialize_color_space(encoder, frame->color_space()));
    return {};
}

template<typename Decoder>
[[nodiscard]] static WebIDL::ExceptionOr<Optional<Gfx::DecodedImageFrame>> deserialize_bitmap(JS::Realm& realm, Decoder& decoder)
{
    auto const kind = TRY(decode_or_throw_data_clone_error<u8>(realm, decoder));
    if (kind == to_underlying(SerializedBitmapKind::None))
        return OptionalNone {};
    if (kind != to_underlying(SerializedBitmapKind::SRGB) && kind != to_underlying(SerializedBitmapKind::WithColorSpace))
        return WebIDL::DataCloneError::create("Invalid ImageBitmap bitmap data"_utf16);
    auto const width = TRY(decode_or_throw_data_clone_error<int>(realm, decoder));
    auto const height = TRY(decode_or_throw_data_clone_error<int>(realm, decoder));
    auto const pitch = TRY(decode_or_throw_data_clone_error<size_t>(realm, decoder));
    auto const format = TRY(decode_or_throw_data_clone_error<Gfx::BitmapFormat>(realm, decoder));
    auto const alpha_type = TRY(decode_or_throw_data_clone_error<Gfx::AlphaType>(realm, decoder));
    auto data = TRY(decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto bitmap = TRY(create_bitmap_from_bitmap_data(realm.vm(), format, alpha_type, width, height, pitch, move(data)));
    Gfx::ColorSpace color_space;
    if (kind == to_underlying(SerializedBitmapKind::WithColorSpace)) {
        auto decoded_color_space = deserialize_color_space(decoder);
        if (decoded_color_space.is_error())
            return data_clone_error_from_serialization_error(realm, decoded_color_space.release_error());
        color_space = decoded_color_space.release_value();
    }
    return Gfx::DecodedImageFrame { *bitmap, move(color_space) };
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

// 2. Set dataHolder.[[BitmapData]] to value's bitmap data, of
// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-steps
static WebIDL::ExceptionOr<void> transfer_bitmap(HTML::TransferDataEncoder& data_holder, ImageBitmapSharedPixels* shared_pixels, Gfx::DecodedImageFrame const* frame)
{
    auto const* bitmap = frame ? &frame->bitmap() : nullptr;
    // NB: The pixels go to the shared memory if there is room for them there, and inline otherwise.
    if (shared_pixels && bitmap && ImageBitmap::shared_pixels_size(*bitmap) <= shared_pixels->buffer.size() - shared_pixels->used_size) {
        auto const pitch = Gfx::Bitmap::minimum_pitch(bitmap->width(), bitmap->format());
        auto* pixels = shared_pixels->buffer.data<u8>() + shared_pixels->used_size;
        for (int y = 0; y < bitmap->height(); ++y)
            memcpy(pixels + y * pitch, bitmap->scanline_u8(y), pitch);

        TRY(encode_bitmap_value(data_holder, true));
        TRY(encode_bitmap_value(data_holder, shared_pixels->buffer_index));
        TRY(encode_bitmap_value(data_holder, static_cast<u64>(shared_pixels->used_size)));
        TRY(encode_bitmap_value(data_holder, bitmap->width()));
        TRY(encode_bitmap_value(data_holder, bitmap->height()));
        TRY(encode_bitmap_value(data_holder, bitmap->format()));
        TRY(encode_bitmap_value(data_holder, bitmap->alpha_type()));
        TRY(serialize_color_space(data_holder, frame->color_space()));
        shared_pixels->used_size += ImageBitmap::shared_pixels_size(*bitmap);
    } else {
        TRY(encode_bitmap_value(data_holder, false));
        TRY(serialize_bitmap(data_holder, frame));
    }
    return {};
}

// 1. Set value's bitmap data to dataHolder.[[BitmapData]], of
// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#the-imagebitmap-interface:transfer-receiving-steps
static WebIDL::ExceptionOr<Optional<Gfx::DecodedImageFrame>> receive_bitmap(JS::Realm& realm, HTML::TransferDataDecoder& data_holder)
{
    auto const has_shared_pixels = TRY(decode_or_throw_data_clone_error<bool>(realm, data_holder));
    if (!has_shared_pixels)
        return TRY(deserialize_bitmap(realm, data_holder));
    auto bitmap = TRY(bitmap_from_shared_pixels(realm.vm(), data_holder));
    auto color_space = deserialize_color_space(data_holder);
    if (color_space.is_error())
        return data_clone_error_from_serialization_error(realm, color_space.release_error());
    return Gfx::DecodedImageFrame { *bitmap, color_space.release_value() };
}

size_t ImageBitmap::shared_pixels_size(Gfx::Bitmap const& bitmap)
{
    auto size = Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(bitmap.width(), bitmap.format()), bitmap.height());
    return round_up_to_power_of_two(size, shared_pixels_alignment);
}

// Hands a decoded frame over to the Rust implementation, which takes it over, or null for none.
static Gfx::DecodedImageFrame* frame_to_hand_over(Optional<Gfx::DecodedImageFrame> frame)
{
    if (!frame.has_value())
        return nullptr;
    return new Gfx::DecodedImageFrame(frame.release_value());
}

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_image_bitmap_create(JSVM*, Gfx::DecodedImageFrame const* frame);
u32 web_image_bitmap_width(void const* image_bitmap);
u32 web_image_bitmap_height(void const* image_bitmap);
bool web_image_bitmap_is_detached(void const* image_bitmap);
void web_image_bitmap_close(void* image_bitmap);
bool web_image_bitmap_origin_clean(void const* image_bitmap);
void web_image_bitmap_set_origin_clean(void* image_bitmap, bool origin_clean);
Gfx::DecodedImageFrame const* web_image_bitmap_frame(void const* image_bitmap);

// What Bindings/Rust/src/html/image_bitmap.rs asks of C++, which owns the encoding of a Gfx::Bitmap: a bitmap that it
// is handed over is a Gfx::DecodedImageFrame allocated with new, or null for none.
size_t web_image_bitmap_frame_size_in_bytes(Gfx::DecodedImageFrame const*);
void web_image_bitmap_serialize_bitmap(Bindings::RustSerializationWriter*, Gfx::DecodedImageFrame const*);
JSCompletion web_image_bitmap_deserialize_bitmap(Bindings::RustSerializationReader*, Gfx::DecodedImageFrame** out_frame);
JSCompletion web_image_bitmap_transfer_bitmap(Bindings::RustTransferDataWriter*, Gfx::DecodedImageFrame const*);
JSCompletion web_image_bitmap_receive_bitmap(Bindings::RustTransferDataReader*, Gfx::DecodedImageFrame** out_frame);

size_t web_image_bitmap_frame_size_in_bytes(Gfx::DecodedImageFrame const* frame)
{
    return frame->bitmap().data_size();
}

void web_image_bitmap_serialize_bitmap(Bindings::RustSerializationWriter* writer, Gfx::DecodedImageFrame const* frame)
{
    MUST(serialize_bitmap(writer->serialized, frame));
}

JSCompletion web_image_bitmap_deserialize_bitmap(Bindings::RustSerializationReader* reader, Gfx::DecodedImageFrame** out_frame)
{
    auto frame = deserialize_bitmap(reader->realm, reader->serialized);
    if (frame.is_exception())
        return Bindings::completion_to_abi(reader->realm, frame.release_error());
    *out_frame = frame_to_hand_over(frame.release_value());
    return Bindings::completion_to_abi(reader->realm, JS::js_undefined());
}

JSCompletion web_image_bitmap_transfer_bitmap(Bindings::RustTransferDataWriter* writer, Gfx::DecodedImageFrame const* frame)
{
    auto result = transfer_bitmap(writer->data_holder, writer->image_bitmap_pixels, frame);
    if (result.is_exception())
        return Bindings::completion_to_abi(writer->realm, result.release_error());
    return Bindings::completion_to_abi(writer->realm, JS::js_undefined());
}

JSCompletion web_image_bitmap_receive_bitmap(Bindings::RustTransferDataReader* reader, Gfx::DecodedImageFrame** out_frame)
{
    auto frame = receive_bitmap(reader->realm, reader->data_holder);
    if (frame.is_exception())
        return Bindings::completion_to_abi(reader->realm, frame.release_error());
    *out_frame = frame_to_hand_over(frame.release_value());
    return Bindings::completion_to_abi(reader->realm, JS::js_undefined());
}
}

GC::Ref<ImageBitmap> ImageBitmap::create(RefPtr<Gfx::Bitmap const> bitmap)
{
    Optional<Gfx::DecodedImageFrame> frame;
    if (bitmap)
        frame = Gfx::DecodedImageFrame { bitmap.release_nonnull() };
    return create(move(frame));
}

GC::Ref<ImageBitmap> ImageBitmap::create(Optional<Gfx::DecodedImageFrame> frame)
{
    auto* image_bitmap = web_image_bitmap_create(JS::HostABI::vm_to_abi(JS::VM::the()), frame.has_value() ? &frame.value() : nullptr);
    return GC::Ref { *static_cast<ImageBitmap*>(image_bitmap) };
}

WebIDL::UnsignedLong ImageBitmap::width() const
{
    return web_image_bitmap_width(this);
}

WebIDL::UnsignedLong ImageBitmap::height() const
{
    return web_image_bitmap_height(this);
}

bool ImageBitmap::is_detached() const
{
    return web_image_bitmap_is_detached(this);
}

void ImageBitmap::close()
{
    web_image_bitmap_close(this);
}

bool ImageBitmap::is_origin_clean() const
{
    return web_image_bitmap_origin_clean(this);
}

void ImageBitmap::set_origin_clean(bool origin_clean)
{
    web_image_bitmap_set_origin_clean(this, origin_clean);
}

Gfx::DecodedImageFrame const* ImageBitmap::frame() const
{
    return web_image_bitmap_frame(this);
}

Gfx::Bitmap const* ImageBitmap::bitmap() const
{
    if (auto const* frame = web_image_bitmap_frame(this))
        return &frame->bitmap();
    return nullptr;
}

}
