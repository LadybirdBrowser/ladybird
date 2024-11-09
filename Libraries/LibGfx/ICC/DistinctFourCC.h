/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Types.h>

namespace Gfx::ICC {

// The ICC spec uses FourCCs for many different things.
// This is used to give FourCCs for different roles distinct types, so that they can only be compared to the correct constants.
// (FourCCs that have only a small and fixed set of values should use an enum class instead, see e.g. DeviceClass and ColorSpace in Enums.h.)
enum class FourCCType {
    PreferredCMMType,
    DeviceManufacturer,
    DeviceModel,
    Creator,
    TagSignature,
    TagTypeSignature,
};

template<FourCCType type>
struct [[gnu::packed]] DistinctFourCC {
    constexpr explicit DistinctFourCC(u32 value)
        : value(value)
    {
    }
    constexpr operator u32() const { return value; }

    char c0() const { return value >> 24; }
    char c1() const { return (value >> 16) & 0xff; }
    char c2() const { return (value >> 8) & 0xff; }
    char c3() const { return value & 0xff; }

    bool operator==(DistinctFourCC b) const { return value == b.value; }

    u32 value { 0 };
};

using PreferredCMMType = DistinctFourCC<FourCCType::PreferredCMMType>;     // ICC v4, "7.2.3 Preferred CMM type field"
using DeviceManufacturer = DistinctFourCC<FourCCType::DeviceManufacturer>; // ICC v4, "7.2.12 Device manufacturer field"
using DeviceModel = DistinctFourCC<FourCCType::DeviceModel>;               // ICC v4, "7.2.13 Device model field"
using Creator = DistinctFourCC<FourCCType::Creator>;                       // ICC v4, "7.2.17 Profile creator field"
using TagSignature = DistinctFourCC<FourCCType::TagSignature>;             // ICC v4, "9.2 Tag listing"
using TagTypeSignature = DistinctFourCC<FourCCType::TagTypeSignature>;     // ICC v4, "10 Tag type definitions"

}

template<Gfx::ICC::FourCCType Type>
struct AK::Formatter<Gfx::ICC::DistinctFourCC<Type>> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::ICC::DistinctFourCC<Type> const& four_cc)
    {
        TRY(builder.put_padding('\'', 1));
        TRY(builder.put_padding(four_cc.c0(), 1));
        TRY(builder.put_padding(four_cc.c1(), 1));
        TRY(builder.put_padding(four_cc.c2(), 1));
        TRY(builder.put_padding(four_cc.c3(), 1));
        TRY(builder.put_padding('\'', 1));
        return {};
    }
};

template<Gfx::ICC::FourCCType Type>
struct AK::Traits<Gfx::ICC::DistinctFourCC<Type>> : public DefaultTraits<Gfx::ICC::DistinctFourCC<Type>> {
    static unsigned hash(Gfx::ICC::DistinctFourCC<Type> const& key)
    {
        return int_hash(key.value);
    }

    static bool equals(Gfx::ICC::DistinctFourCC<Type> const& a, Gfx::ICC::DistinctFourCC<Type> const& b)
    {
        return a == b;
    }
};
