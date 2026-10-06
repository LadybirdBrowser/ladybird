/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkColorSpace.h>

namespace Gfx {

ErrorOr<ColorSpace> ColorSpace::load_from_icc_bytes(ReadonlyBytes icc_bytes)
{
    if (icc_bytes.is_empty())
        return ColorSpace {};

    skcms_ICCProfile icc_profile {};
    if (!skcms_Parse(icc_bytes.data(), icc_bytes.size(), &icc_profile))
        return Error::from_string_literal("Failed to parse the ICC profile");

    auto color_space = SkColorSpace::Make(icc_profile);
    if (!color_space && icc_profile.has_trc && icc_profile.has_toXYZD50) {
        skcms_TransferFunction transfer_function;
        float max_error;
        if (skcms_ApproximateCurve(&icc_profile.trc[0], &transfer_function, &max_error))
            color_space = SkColorSpace::MakeRGB(transfer_function, icc_profile.toXYZD50);
    }
    if (!color_space)
        return ColorSpace {};

    // A Skia color space is exactly a transfer function and a matrix to XYZ D50, so this keeps all of it.
    skcms_TransferFunction transfer_function;
    color_space->transferFn(&transfer_function);
    skcms_Matrix3x3 to_xyz_d50;
    VERIFY(color_space->toXYZD50(&to_xyz_d50));

    Parametric parametric;
    parametric.transfer_function = { transfer_function.g, transfer_function.a, transfer_function.b, transfer_function.c, transfer_function.d, transfer_function.e, transfer_function.f };
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column)
            parametric.to_xyz_d50[row * 3 + column] = to_xyz_d50.vals[row][column];
    }
    return ColorSpace { move(parametric) };
}

static skcms_Matrix3x3 to_xyz_d50_for_color_primaries(Media::ColorPrimaries color_primaries)
{
    auto primaries = [&] {
        switch (color_primaries) {
        case Media::ColorPrimaries::BT709:
        case Media::ColorPrimaries::Unspecified:
            return SkNamedPrimaries::kRec709;
        case Media::ColorPrimaries::BT470M:
            return SkNamedPrimaries::kRec470SystemM;
        case Media::ColorPrimaries::BT470BG:
            return SkNamedPrimaries::kRec470SystemBG;
        case Media::ColorPrimaries::BT601:
            return SkNamedPrimaries::kRec601;
        case Media::ColorPrimaries::SMPTE240:
            return SkNamedPrimaries::kSMPTE_ST_240;
        case Media::ColorPrimaries::GenericFilm:
            return SkNamedPrimaries::kGenericFilm;
        case Media::ColorPrimaries::BT2020:
            return SkNamedPrimaries::kRec2020;
        case Media::ColorPrimaries::SMPTE431:
            return SkNamedPrimaries::kSMPTE_RP_431_2;
        case Media::ColorPrimaries::SMPTE432:
            return SkNamedPrimaries::kSMPTE_EG_432_1;
        case Media::ColorPrimaries::EBU3213:
            return SkNamedPrimaries::kITU_T_H273_Value22;
        case Media::ColorPrimaries::XYZ:
            break;
        }
        VERIFY_NOT_REACHED();
    };

    if (color_primaries == Media::ColorPrimaries::XYZ)
        return SkNamedGamut::kXYZ;
    skcms_Matrix3x3 result;
    VERIFY(primaries().toXYZD50(&result));
    return result;
}

static skcms_TransferFunction transfer_function_for_transfer_characteristics(Media::TransferCharacteristics transfer_characteristics)
{
    switch (transfer_characteristics) {
    case Media::TransferCharacteristics::BT709:
    case Media::TransferCharacteristics::Unspecified:
    case Media::TransferCharacteristics::BT601:
    case Media::TransferCharacteristics::BT2020BitDepth10:
    case Media::TransferCharacteristics::BT2020BitDepth12:
        // This is not technically correct, but other browsers treat these as sRGB as an optimization. The actual
        // transfer characteristics would give output that is not the same as theirs.
        return SkNamedTransferFn::kSRGB;
    case Media::TransferCharacteristics::BT470M:
        return SkNamedTransferFn::kRec470SystemM;
    case Media::TransferCharacteristics::BT470BG:
        return SkNamedTransferFn::kRec470SystemBG;
    case Media::TransferCharacteristics::SMPTE240:
        return SkNamedTransferFn::kSMPTE_ST_240;
    case Media::TransferCharacteristics::Linear:
        return SkNamedTransferFn::kLinear;
    case Media::TransferCharacteristics::IEC61966:
        return SkNamedTransferFn::kIEC61966_2_4;
    case Media::TransferCharacteristics::SRGB:
        return SkNamedTransferFn::kSRGB;
    case Media::TransferCharacteristics::SMPTE2084:
        return SkNamedTransferFn::kPQ;
    case Media::TransferCharacteristics::SMPTE428:
        return SkNamedTransferFn::kSMPTE_ST_428_1;
    case Media::TransferCharacteristics::HLG:
        // FIXME: This will need to change to use the HLG transfer function when the surface we're painting to
        //        supports HDR.
        return SkNamedTransferFn::kSRGB;
    case Media::TransferCharacteristics::Log100:
    case Media::TransferCharacteristics::Log100Sqrt10:
    case Media::TransferCharacteristics::BT1361:
        // ColorSpace::from_cicp() and IPC decoding reject these.
        break;
    }
    VERIFY_NOT_REACHED();
}

static sk_sp<SkColorSpace> create_skia_color_space(ColorSpace::Data const& data)
{
    return data.visit(
        [](Empty) -> sk_sp<SkColorSpace> {
            return nullptr;
        },
        [](ColorSpace::CICP const& cicp) -> sk_sp<SkColorSpace> {
            return SkColorSpace::MakeRGB(transfer_function_for_transfer_characteristics(cicp.transfer_characteristics), to_xyz_d50_for_color_primaries(cicp.color_primaries));
        },
        [](ColorSpace::Parametric const& parametric) -> sk_sp<SkColorSpace> {
            auto const& values = parametric.transfer_function;
            skcms_TransferFunction transfer_function { values[0], values[1], values[2], values[3], values[4], values[5], values[6] };
            skcms_Matrix3x3 to_xyz_d50;
            for (size_t row = 0; row < 3; ++row) {
                for (size_t column = 0; column < 3; ++column)
                    to_xyz_d50.vals[row][column] = parametric.to_xyz_d50[row * 3 + column];
            }
            return SkColorSpace::MakeRGB(transfer_function, to_xyz_d50);
        });
}

static bool have_same_bits(ColorSpace::Data const& a, ColorSpace::Data const& b)
{
    if (a.index() != b.index())
        return false;
    return a.visit(
        [](Empty) { return true; },
        [&](ColorSpace::CICP const& cicp) {
            auto const& other = b.get<ColorSpace::CICP>();
            return cicp.color_primaries == other.color_primaries && cicp.transfer_characteristics == other.transfer_characteristics;
        },
        [&](ColorSpace::Parametric const& parametric) {
            auto const& other = b.get<ColorSpace::Parametric>();
            return __builtin_memcmp(parametric.transfer_function.data(), other.transfer_function.data(), sizeof(parametric.transfer_function)) == 0
                && __builtin_memcmp(parametric.to_xyz_d50.data(), other.to_xyz_d50.data(), sizeof(parametric.to_xyz_d50)) == 0;
        });
}

sk_sp<SkColorSpace> to_skia_color_space(ColorSpace const& color_space)
{
    if (color_space.data().has<Empty>())
        return nullptr;

    // Images in one color space are drawn many times, and Skia makes a new color space object for each request that
    // is not sRGB. So keep the color spaces that were made last.
    struct CachedColorSpace {
        ColorSpace::Data data;
        sk_sp<SkColorSpace> skia_color_space;
    };
    struct Cache {
        Mutex mutex;
        Vector<CachedColorSpace> entries;
    };
    static constexpr size_t max_cached_color_spaces = 32;
    static NeverDestroyed<Cache> cache;

    MutexLocker locker(cache->mutex);
    for (auto const& entry : cache->entries) {
        if (have_same_bits(entry.data, color_space.data()))
            return entry.skia_color_space;
    }
    if (cache->entries.size() >= max_cached_color_spaces)
        cache->entries.clear();
    auto skia_color_space = create_skia_color_space(color_space.data());
    cache->entries.append({ color_space.data(), skia_color_space });
    return skia_color_space;
}

}
