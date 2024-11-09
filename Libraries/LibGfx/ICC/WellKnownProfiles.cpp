/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ICC/Profile.h>
#include <LibGfx/ICC/Tags.h>
#include <LibGfx/ICC/WellKnownProfiles.h>
#include <time.h>

namespace Gfx::ICC {

static ProfileHeader rgb_header()
{
    ProfileHeader header;
    header.version = Version(4, 0x40);
    header.device_class = DeviceClass::DisplayDevice;
    header.data_color_space = ColorSpace::RGB;
    header.connection_space = ColorSpace::PCSXYZ;
    header.creation_timestamp = MUST(DateTime::from_time_t(0));
    header.rendering_intent = RenderingIntent::Perceptual;
    header.pcs_illuminant = XYZ { 0.9642, 1.0, 0.8249 };
    return header;
}

static ErrorOr<NonnullRefPtr<MultiLocalizedUnicodeTagData>> en_US(StringView text)
{
    Vector<MultiLocalizedUnicodeTagData::Record> records;
    TRY(records.try_append({ ('e' << 8) | 'n', ('U' << 8) | 'S', TRY(String::from_utf8(text)) }));
    return try_make_ref_counted<MultiLocalizedUnicodeTagData>(0, 0, records);
}

static ErrorOr<NonnullRefPtr<XYZTagData>> XYZ_data(XYZ xyz)
{
    Vector<XYZ> xyzs;
    TRY(xyzs.try_append(xyz));
    return try_make_ref_counted<XYZTagData>(0, 0, move(xyzs));
}

ErrorOr<NonnullRefPtr<TagData>> sRGB_curve()
{
    // Numbers from https://en.wikipedia.org/wiki/SRGB#From_sRGB_to_CIE_XYZ
    Array<S15Fixed16, 7> curve_parameters = { 2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045 };
    return try_make_ref_counted<ParametricCurveTagData>(0, 0, ParametricCurveTagData::FunctionType::sRGB, curve_parameters);
}

ErrorOr<NonnullRefPtr<Profile>> sRGB()
{
    // Returns an sRGB profile.
    // https://en.wikipedia.org/wiki/SRGB

    // FIXME: There are many different sRGB ICC profiles in the wild.
    //        Explain why, and why this picks the numbers it does.
    //        In the meantime, https://github.com/SerenityOS/serenity/pull/17714 has a few notes.

    auto header = rgb_header();

    OrderedHashMap<TagSignature, NonnullRefPtr<TagData>> tag_table;

    TRY(tag_table.try_set(profileDescriptionTag, TRY(en_US("SerenityOS sRGB"sv))));
    TRY(tag_table.try_set(copyrightTag, TRY(en_US("Public Domain"sv))));

    // Transfer function.
    auto curve = TRY(sRGB_curve());
    TRY(tag_table.try_set(redTRCTag, curve));
    TRY(tag_table.try_set(greenTRCTag, curve));
    TRY(tag_table.try_set(blueTRCTag, curve));

    // White point.
    // ICC v4, 9.2.36 mediaWhitePointTag: "For displays, the values specified shall be those of the PCS illuminant as defined in 7.2.16."
    TRY(tag_table.try_set(mediaWhitePointTag, TRY(XYZ_data(header.pcs_illuminant))));

    // The chromatic_adaptation_matrix values are from https://www.color.org/chadtag.xalter
    // That leads to exactly the S15Fixed16 values in the sRGB profiles in GIMP, Android, RawTherapee (but not in Compact-ICC-Profiles's v4 sRGB profile).
    Vector<S15Fixed16, 9> chromatic_adaptation_matrix = { 1.047882, 0.022918, -0.050217, 0.029586, 0.990478, -0.017075, -0.009247, 0.015075, 0.751678 };
    TRY(tag_table.try_set(chromaticAdaptationTag, TRY(try_make_ref_counted<S15Fixed16ArrayTagData>(0, 0, move(chromatic_adaptation_matrix)))));

    // The chromaticity values are from https://www.color.org/srgb.pdf
    // The chromatic adaptation matrix in that document is slightly different from the one on https://www.color.org/chadtag.xalter,
    // so the values in our sRGB profile are currently not fully self-consistent.
    // FIXME: Make values self-consistent (probably by using slightly different chromaticities).
    TRY(tag_table.try_set(redMatrixColumnTag, TRY(XYZ_data(XYZ { 0.436030342570117, 0.222438466210245, 0.013897440074263 }))));
    TRY(tag_table.try_set(greenMatrixColumnTag, TRY(XYZ_data(XYZ { 0.385101860087134, 0.716942745571917, 0.097076381494207 }))));
    TRY(tag_table.try_set(blueMatrixColumnTag, TRY(XYZ_data(XYZ { 0.143067806654203, 0.060618777416563, 0.713926257896652 }))));

    return Profile::create(header, move(tag_table));
}

}
