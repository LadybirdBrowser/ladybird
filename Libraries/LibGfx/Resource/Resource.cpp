/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Resource/Resource.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

static u16 s_default_resource_id_client;

StringView ResourceID::type_name(WireResourceType type)
{
    switch (type) {
    case WireResourceType::SkTypeface:
        return "SkTypeface"sv;
    case WireResourceType::Bitmap:
        return "Bitmap"sv;
    case WireResourceType::LocalFont:
        return "LocalFont"sv;
    default:
    }
    return "Invalid"sv;
}

u16 ResourceID::this_client()
{
    return s_default_resource_id_client;
}

void ResourceID::set_this_client(u16 client_id)
{
    s_default_resource_id_client = client_id;
}

ResourceTransfer::ResourceTransfer() = default;
ResourceTransfer::ResourceTransfer(ResourceTransfer&&) = default;
ResourceTransfer& ResourceTransfer::operator=(ResourceTransfer&&) = default;
ResourceTransfer::~ResourceTransfer() = default;

}

namespace AK {

ErrorOr<void> Formatter<Gfx::WireResourceType>::format(FormatBuilder& builder, Gfx::WireResourceType const& value)
{
    return Formatter<StringView>::format(builder, Gfx::ResourceID::type_name(value));
}

ErrorOr<void> Formatter<Gfx::ResourceID>::format(FormatBuilder& builder, Gfx::ResourceID const& value)
{
    TRY(builder.put_literal("id="sv));
    TRY(Formatter<u32> {}.format(builder, value.id()));
    TRY(builder.put_literal(" type="sv));
    TRY(Formatter<Gfx::WireResourceType> {}.format(builder, value.type()));
    TRY(builder.put_literal(" client="sv));
    TRY(Formatter<unsigned> {}.format(builder, value.client()));
    if (value.flags() != 0) {
        TRY(builder.put_literal(" flags="sv));
        TRY(Formatter<unsigned> {}.format(builder, value.flags()));
    }
    return {};
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ResourceID const& value)
{
    return encoder.encode(value.raw());
}

template<>
ErrorOr<Gfx::ResourceID> decode(Decoder& decoder)
{
    return Gfx::ResourceID::from_raw(TRY(decoder.decode<u64>()));
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::ResourceInfo const& descriptor)
{
    TRY(encoder.encode(descriptor.resource_id));
    TRY(encoder.encode(descriptor.font.ttc_index));
    return {};
}

template<>
ErrorOr<Gfx::ResourceInfo> decode(Decoder& decoder)
{
    Gfx::ResourceInfo descriptor;
    descriptor.resource_id = TRY(decoder.decode<Gfx::ResourceID>());
    descriptor.font.ttc_index = TRY(decoder.decode<u32>());
    return descriptor;
}

}
