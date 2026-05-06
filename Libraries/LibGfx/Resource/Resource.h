/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/HashFunctions.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>

namespace Gfx {

class SharedImagePayload;

using ReleaseToken = u64;
using ImageID = u64;

enum class WireResourceType : u8 {
    Invalid = 0,
    Bitmap = 1,
    SkTypeface = 2,
    LocalFont = 3,
};

struct ResourceID {
    static constexpr u64 TYPE_MASK = 0xff00000000000000ull;
    static constexpr u64 ID_MASK = 0x00000000ffffffffull;
    static constexpr u64 CLIENT_MASK = 0x0000ffff00000000ull;
    static constexpr u64 FLAGS_MASK = 0x00ff000000000000ull; // aka reserved

    static StringView type_name(WireResourceType);
    static u16 this_client();
    static void set_this_client(u16 client_id);

    static constexpr ResourceID from_raw(u64 value) { return { value }; }
    static constexpr ResourceID make(WireResourceType type, u32 id, u8 flags = 0)
    {
        return from_raw((static_cast<u64>(to_underlying(type)) << 56)
            | (static_cast<u64>(flags) << 48)
            | (static_cast<u64>(this_client()) << 32)
            | id);
    }

    constexpr u32 id() const { return static_cast<u32>(m_value & ID_MASK); }
    constexpr WireResourceType type() const { return static_cast<WireResourceType>((m_value & TYPE_MASK) >> 56); }
    constexpr u16 client() const { return static_cast<u16>((m_value & CLIENT_MASK) >> 32); }
    constexpr u8 flags() const { return static_cast<u8>((m_value & FLAGS_MASK) >> 48); }
    constexpr u64 raw() const { return m_value; }

    constexpr bool operator==(ResourceID const&) const = default;

    u64 m_value;
};

constexpr bool operator==(ResourceID lhs, u64 rhs) { return lhs.raw() == rhs; }
constexpr bool operator==(u64 lhs, ResourceID rhs) { return lhs == rhs.raw(); }

struct ResourceInfo {
    ResourceID resource_id { 0 };
    struct FontData {
        u32 ttc_index { 0 };
    } font;
};

struct ResourceTransfer {
    ResourceTransfer();
    ResourceTransfer(ResourceTransfer&&);
    ResourceTransfer& operator=(ResourceTransfer&&);
    ~ResourceTransfer();

    ResourceInfo info;
    ReadonlyBytes bytes;
    OwnPtr<SharedImagePayload> shared_image_payload;
};

}

template<>
struct AK::Traits<Gfx::ResourceID> : public AK::DefaultTraits<Gfx::ResourceID> {
    static constexpr bool is_trivial() { return true; }
    static constexpr bool is_trivially_serializable() { return true; }
    static constexpr bool may_have_slow_equality_check() { return false; }
    static u32 hash(Gfx::ResourceID value)
    {
        return u64_hash(value.raw());
    }
};

static_assert(sizeof(Gfx::ResourceID) == sizeof(u64));
static_assert(alignof(Gfx::ResourceID) == alignof(u64));
static_assert(IsTrivial<Gfx::ResourceID>);
static_assert(IsTriviallyCopyable<Gfx::ResourceID>);
static_assert(IsTriviallyDestructible<Gfx::ResourceID>);
static_assert(__is_standard_layout(Gfx::ResourceID));

template<>
struct AK::Formatter<Gfx::WireResourceType> : AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Gfx::WireResourceType const&);
};

template<>
struct AK::Formatter<Gfx::ResourceID> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder&, Gfx::ResourceID const&);
};

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ResourceID const&);

template<>
ErrorOr<Gfx::ResourceID> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::ResourceInfo const&);

template<>
ErrorOr<Gfx::ResourceInfo> decode(Decoder&);

}
