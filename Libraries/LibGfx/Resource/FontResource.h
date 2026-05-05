/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Font/TypefaceSkia.h>
#include <LibGfx/Resource/Resource.h>

namespace Gfx {

class Font;

struct FontResourceKey {
    u32 typeface_unique_id { 0 };
    unsigned ttc_index { 0 };
    WireResourceType resource_type { WireResourceType::SkTypeface };

    bool operator==(FontResourceKey const& other) const = default;
};

struct DecodedSkTypefacePayload {
    ReadonlyBytes typeface_bytes;
    u32 ttc_index { 0 };
    Vector<FontVariationAxis> variation_axes;
};

ErrorOr<ByteBuffer> local_font_info_to_bytes(TypefaceSkia::LocalFontInfo const&);
ErrorOr<TypefaceSkia::LocalFontInfo> bytes_to_local_font_info(ReadonlyBytes);
bool has_skia_typeface_payload_header(ReadonlyBytes);
ErrorOr<ByteBuffer> skia_typeface_to_bytes(TypefaceSkia const&);
ErrorOr<DecodedSkTypefacePayload> bytes_to_skia_typeface_payload(ReadonlyBytes);

class FontResourceRegistry {
public:
    // Returns the font resource ID for a typeface, registering it if new.
    // Newly registered typefaces are added to the pending submission list.
    ResourceID ensure_font_resource(Font const& font);

    // Take the list of font registrations that need to be sent this frame.
    Vector<ResourceTransfer> take_pending_transfers();

    void invalidate_resource(ResourceID resource_id);

    void clear();

private:
    HashMap<TypefaceSkia const*, ResourceID> m_typeface_to_resource_id;
    HashMap<FontResourceKey, ResourceID> m_identity_to_resource_id;
    HashMap<ResourceID, NonnullOwnPtr<ByteBuffer>> m_typeface_payloads;
    HashMap<ResourceID, NonnullOwnPtr<ByteBuffer>> m_local_font_payloads;
    Vector<ResourceTransfer> m_pending_transfers;
    u32 m_next_resource_id_seed { 1 };
};

}

template<>
struct AK::Traits<Gfx::FontResourceKey> : public AK::DefaultTraits<Gfx::FontResourceKey> {
    static unsigned hash(Gfx::FontResourceKey const& identity)
    {
        return pair_int_hash(pair_int_hash(identity.typeface_unique_id, identity.ttc_index), to_underlying(identity.resource_type));
    }
};
