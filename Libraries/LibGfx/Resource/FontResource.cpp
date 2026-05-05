/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/ByteBuffer.h>
#include <AK/Checked.h>
#include <AK/Endian.h>
#include <AK/TypeCasts.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibGfx/Resource/FontResource.h>
#include <core/SkTypeface.h>

namespace Gfx {

struct LocalFontPayloadHeader {
    u32 resource_name_length { 0 };
    u32 family_name_length { 0 };
    u32 variation_axis_count { 0 };
    u32 ttc_index { 0 };
    u16 weight { 0 };
    u16 width { 0 };
    u8 slope { 0 };
    u8 padding[3] { 0, 0, 0 };
};

struct [[gnu::packed]] LocalFontPayloadAxis {
    u32 tag { 0 };
    u32 value_bits { 0 };
};

struct SkTypefacePayloadHeader {
    u32 magic { 0 };
    u32 font_data_size { 0 };
    u32 variation_axis_count { 0 };
    u32 ttc_index { 0 };
};

static constexpr u32 SKIA_TYPEFACE_PAYLOAD_MAGIC = 0x46544753;

static FontResourceKey font_resource_key(TypefaceSkia const& typeface)
{
    return FontResourceKey {
        .typeface_unique_id = typeface.sk_typeface()->uniqueID(),
        .ttc_index = typeface.ttc_index(),
        .resource_type = typeface.local_font_info().has_value() ? WireResourceType::LocalFont : WireResourceType::SkTypeface,
    };
}

ErrorOr<ByteBuffer> local_font_info_to_bytes(TypefaceSkia::LocalFontInfo const& identifier)
{
    Checked<size_t> total_size = sizeof(LocalFontPayloadHeader);
    total_size += identifier.resource_name.length();
    total_size += identifier.family_name.length();
    total_size += identifier.variation_axes.size() * sizeof(LocalFontPayloadAxis);
    if (total_size.has_overflow())
        return Error::from_string_literal("LocalFont payload is too large");

    auto payload = TRY(ByteBuffer::create_uninitialized(total_size.value()));

    LocalFontPayloadHeader header;
    header.resource_name_length = static_cast<u32>(identifier.resource_name.length());
    header.family_name_length = static_cast<u32>(identifier.family_name.length());
    header.variation_axis_count = static_cast<u32>(identifier.variation_axes.size());
    header.ttc_index = identifier.ttc_index;
    header.weight = identifier.weight;
    header.width = identifier.width;
    header.slope = identifier.slope;
    payload.overwrite(0, &header, sizeof(header));

    size_t offset = sizeof(header);
    if (!identifier.resource_name.is_empty()) {
        payload.overwrite(offset, identifier.resource_name.bytes().data(), identifier.resource_name.length());
        offset += identifier.resource_name.length();
    }
    if (!identifier.family_name.is_empty()) {
        payload.overwrite(offset, identifier.family_name.bytes().data(), identifier.family_name.length());
        offset += identifier.family_name.length();
    }
    for (auto const& axis : identifier.variation_axes) {
        LocalFontPayloadAxis axis_payload;
        axis_payload.tag = axis.tag.to_u32();
        axis_payload.value_bits = bit_cast<u32>(axis.value);
        payload.overwrite(offset, &axis_payload, sizeof(axis_payload));
        offset += sizeof(axis_payload);
    }

    return payload;
}

ErrorOr<TypefaceSkia::LocalFontInfo> bytes_to_local_font_info(ReadonlyBytes bytes)
{
    if (bytes.size() < sizeof(LocalFontPayloadHeader))
        return Error::from_string_literal("LocalFont payload is too small");

    LocalFontPayloadHeader header;
    __builtin_memcpy(&header, bytes.data(), sizeof(header));

    Checked<size_t> offset = sizeof(LocalFontPayloadHeader);
    offset += header.resource_name_length;
    offset += header.family_name_length;
    offset += header.variation_axis_count * sizeof(LocalFontPayloadAxis);
    if (offset.has_overflow() || offset.value() != bytes.size())
        return Error::from_string_literal("LocalFont payload is malformed");

    size_t read_offset = sizeof(LocalFontPayloadHeader);
    ByteString resource_name = ByteString { StringView { bytes.data() + read_offset, header.resource_name_length } };
    read_offset += header.resource_name_length;

    ByteString family_name = ByteString { StringView { bytes.data() + read_offset, header.family_name_length } };
    read_offset += header.family_name_length;

    Vector<FontVariationAxis> variation_axes;
    TRY(variation_axes.try_ensure_capacity(header.variation_axis_count));
    for (u32 index = 0; index < header.variation_axis_count; ++index) {
        LocalFontPayloadAxis axis_payload;
        __builtin_memcpy(&axis_payload, bytes.data() + read_offset, sizeof(axis_payload));
        TRY(variation_axes.try_append(FontVariationAxis {
            FourCC::from_u32(axis_payload.tag),
            bit_cast<float>(axis_payload.value_bits),
        }));
        read_offset += sizeof(axis_payload);
    }

    return TypefaceSkia::LocalFontInfo {
        .resource_name = move(resource_name),
        .family_name = move(family_name),
        .weight = header.weight,
        .width = header.width,
        .slope = header.slope,
        .ttc_index = header.ttc_index,
        .variation_axes = move(variation_axes),
    };
}

bool has_skia_typeface_payload_header(ReadonlyBytes bytes)
{
    if (bytes.size() < sizeof(SkTypefacePayloadHeader))
        return false;

    SkTypefacePayloadHeader header;
    __builtin_memcpy(&header, bytes.data(), sizeof(header));
    return header.magic == SKIA_TYPEFACE_PAYLOAD_MAGIC;
}

ErrorOr<ByteBuffer> skia_typeface_to_bytes(TypefaceSkia const& typeface)
{
    auto const& typeface_bytes = typeface.buffer();
    auto const& variation_axes = typeface.variation_axes();

    Checked<size_t> total_size = sizeof(SkTypefacePayloadHeader);
    total_size += typeface_bytes.size();
    total_size += variation_axes.size() * sizeof(LocalFontPayloadAxis);
    if (total_size.has_overflow())
        return Error::from_string_literal("SkTypeface payload is too large");

    auto payload = TRY(ByteBuffer::create_uninitialized(total_size.value()));

    SkTypefacePayloadHeader header;
    header.magic = SKIA_TYPEFACE_PAYLOAD_MAGIC;
    header.font_data_size = static_cast<u32>(typeface_bytes.size());
    header.variation_axis_count = static_cast<u32>(variation_axes.size());
    header.ttc_index = typeface.ttc_index();
    payload.overwrite(0, &header, sizeof(header));

    size_t offset = sizeof(header);
    for (auto const& axis : variation_axes) {
        LocalFontPayloadAxis axis_payload;
        axis_payload.tag = axis.tag.to_u32();
        axis_payload.value_bits = bit_cast<u32>(axis.value);
        payload.overwrite(offset, &axis_payload, sizeof(axis_payload));
        offset += sizeof(axis_payload);
    }

    if (!typeface_bytes.is_empty())
        payload.overwrite(offset, typeface_bytes.data(), typeface_bytes.size());

    return payload;
}

ErrorOr<DecodedSkTypefacePayload> bytes_to_skia_typeface_payload(ReadonlyBytes bytes)
{
    if (bytes.size() < sizeof(SkTypefacePayloadHeader))
        return Error::from_string_literal("SkTypeface payload is too small");

    SkTypefacePayloadHeader header;
    __builtin_memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != SKIA_TYPEFACE_PAYLOAD_MAGIC)
        return Error::from_string_literal("SkTypeface payload has invalid magic");

    Checked<size_t> offset = sizeof(SkTypefacePayloadHeader);
    offset += header.variation_axis_count * sizeof(LocalFontPayloadAxis);
    offset += header.font_data_size;
    if (offset.has_overflow() || offset.value() != bytes.size())
        return Error::from_string_literal("SkTypeface payload is malformed");

    size_t read_offset = sizeof(SkTypefacePayloadHeader);

    Vector<FontVariationAxis> variation_axes;
    TRY(variation_axes.try_ensure_capacity(header.variation_axis_count));
    for (u32 index = 0; index < header.variation_axis_count; ++index) {
        LocalFontPayloadAxis axis_payload;
        __builtin_memcpy(&axis_payload, bytes.data() + read_offset, sizeof(axis_payload));
        TRY(variation_axes.try_append(FontVariationAxis {
            FourCC::from_u32(axis_payload.tag),
            bit_cast<float>(axis_payload.value_bits),
        }));
        read_offset += sizeof(axis_payload);
    }

    return DecodedSkTypefacePayload {
        .typeface_bytes = bytes.slice(read_offset, header.font_data_size),
        .ttc_index = header.ttc_index,
        .variation_axes = move(variation_axes),
    };
}

ResourceID FontResourceRegistry::ensure_font_resource(Font const& font)
{
    auto const& typeface = as<TypefaceSkia>(font.typeface());
    auto existing = m_typeface_to_resource_id.get(&typeface);
    if (existing.has_value())
        return existing.value();

    FontResourceKey const identity = font_resource_key(typeface);
    existing = m_identity_to_resource_id.get(identity);
    if (existing.has_value()) {
        m_typeface_to_resource_id.set(&typeface, existing.value());
        return existing.value();
    }

    WireResourceType const resource_type = typeface.local_font_info().has_value() ? WireResourceType::LocalFont : WireResourceType::SkTypeface;
    ResourceID resource_id = ResourceID::make(resource_type, m_next_resource_id_seed++);
    m_typeface_to_resource_id.set(&typeface, resource_id);
    m_identity_to_resource_id.set(identity, resource_id);

    ResourceInfo descriptor;
    descriptor.resource_id = resource_id;
    descriptor.font.ttc_index = typeface.ttc_index();

    ReadonlyBytes payload_bytes;
    if (auto local_font_info = typeface.local_font_info(); local_font_info.has_value()) {
        auto payload = MUST(local_font_info_to_bytes(*local_font_info));
        auto payload_storage = make<ByteBuffer>(move(payload));
        payload_bytes = payload_storage->bytes();
        m_local_font_payloads.set(resource_id, move(payload_storage));
    } else {
        auto payload = MUST(skia_typeface_to_bytes(typeface));
        auto payload_storage = make<ByteBuffer>(move(payload));
        payload_bytes = payload_storage->bytes();
        m_typeface_payloads.set(resource_id, move(payload_storage));
    }

    ResourceTransfer transfer;
    transfer.info = descriptor;
    transfer.bytes = payload_bytes;

    m_pending_transfers.append(move(transfer));

    return resource_id;
}

Vector<ResourceTransfer> FontResourceRegistry::take_pending_transfers()
{
    return move(m_pending_transfers);
}

void FontResourceRegistry::invalidate_resource(ResourceID resource_id)
{
    if (resource_id == 0)
        return;

    Vector<TypefaceSkia const*> typefaces_to_remove;
    for (auto const& it : m_typeface_to_resource_id) {
        if (it.value == resource_id)
            typefaces_to_remove.append(it.key);
    }
    for (auto const* typeface : typefaces_to_remove)
        m_typeface_to_resource_id.remove(typeface);

    Vector<FontResourceKey> identities_to_remove;
    for (auto const& it : m_identity_to_resource_id) {
        if (it.value == resource_id)
            identities_to_remove.append(it.key);
    }
    for (auto const& identity : identities_to_remove)
        m_identity_to_resource_id.remove(identity);

    m_typeface_payloads.remove(resource_id);
    m_local_font_payloads.remove(resource_id);

    for (size_t index = m_pending_transfers.size(); index-- > 0;) {
        if (m_pending_transfers[index].info.resource_id == resource_id)
            m_pending_transfers.remove(index);
    }
}

void FontResourceRegistry::clear()
{
    m_typeface_to_resource_id.clear();
    m_identity_to_resource_id.clear();
    m_typeface_payloads.clear();
    m_local_font_payloads.clear();
    m_pending_transfers.clear();
}

}
