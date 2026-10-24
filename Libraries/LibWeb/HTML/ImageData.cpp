/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWebCommon/Bindings/PredefinedColorSpace.h>

namespace Web::HTML {

static_assert(to_underlying(Bindings::PredefinedColorSpace::Srgb) == 0);
static_assert(to_underlying(Bindings::PredefinedColorSpace::SrgbLinear) == 1);
static_assert(to_underlying(Bindings::PredefinedColorSpace::DisplayP3) == 2);
static_assert(to_underlying(Bindings::PredefinedColorSpace::DisplayP3Linear) == 3);

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
JSCompletion web_image_data_create(JSVM*, void* realm, u32 width, u32 height, void** out_image_data);
u32 web_image_data_width(void const* image_data);
u32 web_image_data_height(void const* image_data);
bool web_image_data_is_detached(void const* image_data);
u8* web_image_data_pixels(void const* image_data);
u8 web_image_data_color_space(void const* image_data);

// What the serialization steps of Bindings/Rust/src/html/image_data.rs ask of C++: the color space, encoded as for
// any other Bindings::PredefinedColorSpace.
void web_image_data_serialize_color_space(Bindings::RustSerializationWriter*, Bindings::PredefinedColorSpace);
JSCompletion web_image_data_deserialize_color_space(Bindings::RustSerializationReader*, Bindings::PredefinedColorSpace* out_color_space);

void web_image_data_serialize_color_space(Bindings::RustSerializationWriter* writer, Bindings::PredefinedColorSpace color_space)
{
    writer->serialized.encode(color_space);
}

JSCompletion web_image_data_deserialize_color_space(Bindings::RustSerializationReader* reader, Bindings::PredefinedColorSpace* out_color_space)
{
    auto color_space = decode_or_throw_data_clone_error<Bindings::PredefinedColorSpace>(reader->realm, reader->serialized);
    if (color_space.is_exception())
        return Bindings::completion_to_abi(reader->realm, color_space.release_error());
    *out_color_space = color_space.release_value();
    return Bindings::completion_to_abi(reader->realm, JS::js_undefined());
}
}

WebIDL::ExceptionOr<GC::Ref<ImageData>> ImageData::create(JS::Realm& realm, u32 width, u32 height)
{
    void* image_data = nullptr;
    TRY(JS::HostABI::completion_from_abi<void>(web_image_data_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, width, height, &image_data)));
    return GC::Ref { *static_cast<ImageData*>(image_data) };
}

u32 ImageData::width() const
{
    return web_image_data_width(this);
}

u32 ImageData::height() const
{
    return web_image_data_height(this);
}

bool ImageData::is_detached() const
{
    return web_image_data_is_detached(this);
}

Bindings::PredefinedColorSpace ImageData::color_space() const
{
    return static_cast<Bindings::PredefinedColorSpace>(web_image_data_color_space(this));
}

u8* ImageData::pixels() const
{
    return web_image_data_pixels(this);
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> ImageData::bitmap() const
{
    auto* pixels = this->pixels();
    if (!pixels)
        return Error::from_errno(EINVAL);
    Gfx::IntSize size { static_cast<int>(width()), static_cast<int>(height()) };
    return Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, size, size.width() * sizeof(u32), pixels);
}

}
