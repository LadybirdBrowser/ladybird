/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/ICC/BinaryWriter.h>
#include <LibGfx/ICC/Profile.h>
#include <LibGfx/ICC/Tags.h>
#include <LibGfx/ICC/WellKnownProfiles.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/WebPLoader.h>
#include <LibTest/TestCase.h>

#define TEST_INPUT(x) ("test-inputs/" x)

TEST_CASE(png)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("icc/icc-v2.png"sv)));
    auto png = MUST(Gfx::PNGImageDecoderPlugin::create(file->bytes()));
    auto icc_bytes = MUST(png->icc_data());
    EXPECT(icc_bytes.has_value());

    auto icc_profile = MUST(Gfx::ICC::Profile::try_load_from_externally_owned_memory(icc_bytes.value()));
    EXPECT(icc_profile->is_v2());
}

TEST_CASE(jpg)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("icc/icc-v4.jpg"sv)));
    auto jpg = MUST(Gfx::JPEGImageDecoderPlugin::create(file->bytes()));
    auto icc_bytes = MUST(jpg->icc_data());
    EXPECT(icc_bytes.has_value());

    auto icc_profile = MUST(Gfx::ICC::Profile::try_load_from_externally_owned_memory(icc_bytes.value()));
    EXPECT(icc_profile->is_v4());

    icc_profile->for_each_tag([](auto tag_signature, auto tag_data) {
        if (tag_signature == Gfx::ICC::profileDescriptionTag) {
            // Required per v4 spec, but in practice even v4 files sometimes have TextDescriptionTagData descriptions. Not icc-v4.jpg, though.
            EXPECT_EQ(tag_data->type(), Gfx::ICC::MultiLocalizedUnicodeTagData::Type);
            auto& multi_localized_unicode = static_cast<Gfx::ICC::MultiLocalizedUnicodeTagData&>(*tag_data);
            EXPECT_EQ(multi_localized_unicode.records().size(), 1u);
            auto& record = multi_localized_unicode.records()[0];
            EXPECT_EQ(record.text, "sRGB built-in"sv);
        }
    });
}

TEST_CASE(webp_extended_lossless)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("icc/extended-lossless.webp"sv)));
    auto webp = MUST(Gfx::WebPImageDecoderPlugin::create(file->bytes()));
    auto icc_bytes = MUST(webp->icc_data());
    EXPECT(icc_bytes.has_value());

    auto icc_profile = MUST(Gfx::ICC::Profile::try_load_from_externally_owned_memory(icc_bytes.value()));
    EXPECT(icc_profile->is_v2());
}

TEST_CASE(webp_extended_lossy)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("icc/extended-lossy.webp"sv)));
    auto webp = MUST(Gfx::WebPImageDecoderPlugin::create(file->bytes()));
    auto icc_bytes = MUST(webp->icc_data());
    EXPECT(icc_bytes.has_value());

    auto icc_profile = MUST(Gfx::ICC::Profile::try_load_from_externally_owned_memory(icc_bytes.value()));
    EXPECT(icc_profile->is_v2());
}

TEST_CASE(tiff)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("icc/icc.tiff"sv)));
    auto tiff = MUST(Gfx::TIFFImageDecoderPlugin::create(file->bytes()));
    auto icc_bytes = MUST(tiff->icc_data());
    EXPECT(icc_bytes.has_value());

    auto icc_profile = MUST(Gfx::ICC::Profile::try_load_from_externally_owned_memory(icc_bytes.value()));
    EXPECT(icc_profile->is_v4());
}

TEST_CASE(serialize_icc)
{
    auto file = MUST(Core::MappedFile::map(TEST_INPUT("icc/p3-v4.icc"sv)));
    auto icc_profile = MUST(Gfx::ICC::Profile::try_load_from_externally_owned_memory(file->bytes()));
    EXPECT(icc_profile->is_v4());

    auto serialized_bytes = MUST(Gfx::ICC::encode(*icc_profile));
    EXPECT_EQ(serialized_bytes, file->bytes());
}

TEST_CASE(built_in_sRGB)
{
    auto sRGB = MUST(Gfx::ICC::sRGB());
    auto serialized_bytes = MUST(Gfx::ICC::encode(sRGB));

    // We currently exactly match the curve in GIMP's built-in sRGB profile. It's a type 3 'para' curve with 5 parameters.
    u32 para[] = { 0x70617261, 0x00000000, 0x00030000, 0x00026666, 0x0000F2A7, 0x00000D59, 0x000013D0, 0x00000A5B };
    for (u32& i : para)
        i = AK::convert_between_host_and_big_endian(i);
    EXPECT(memmem(serialized_bytes.data(), serialized_bytes.size(), para, sizeof(para)) != nullptr);

    // We currently exactly match the chromatic adaptation matrix in GIMP's (and other's) built-in sRGB profile.
    u32 sf32[] = { 0x73663332, 0x00000000, 0x00010C42, 0x000005DE, 0xFFFFF325, 0x00000793, 0x0000FD90, 0xFFFFFBA1, 0xFFFFFDA2, 0x000003DC, 0x0000C06E };
    for (u32& i : sf32)
        i = AK::convert_between_host_and_big_endian(i);
    EXPECT(memmem(serialized_bytes.data(), serialized_bytes.size(), sf32, sizeof(sf32)) != nullptr);
}

TEST_CASE(to_pcs)
{
    auto sRGB = MUST(Gfx::ICC::sRGB());
    EXPECT(sRGB->data_color_space() == Gfx::ICC::ColorSpace::RGB);
    EXPECT(sRGB->connection_space() == Gfx::ICC::ColorSpace::PCSXYZ);

    auto sRGB_curve_pointer = MUST(Gfx::ICC::sRGB_curve());
    VERIFY(sRGB_curve_pointer->type() == Gfx::ICC::ParametricCurveTagData::Type);
    auto const& sRGB_curve = static_cast<Gfx::ICC::ParametricCurveTagData const&>(*sRGB_curve_pointer);
    EXPECT_EQ(sRGB_curve.evaluate(0.f), 0.f);
    EXPECT_EQ(sRGB_curve.evaluate(1.f), 1.f);

    auto xyz_from_sRGB = [&sRGB](u8 r, u8 g, u8 b) {
        u8 rgb[3] = { r, g, b };
        return MUST(sRGB->to_pcs(rgb));
    };

    auto vec3_from_xyz = [](Gfx::ICC::XYZ const& xyz) {
        return FloatVector3 { xyz.X, xyz.Y, xyz.Z };
    };

#define EXPECT_APPROXIMATE_VECTOR3(v1, v2) \
    EXPECT_APPROXIMATE((v1)[0], (v2)[0]);  \
    EXPECT_APPROXIMATE((v1)[1], (v2)[1]);  \
    EXPECT_APPROXIMATE((v1)[2], (v2)[2]);

    // At 0 and 255, the gamma curve is (exactly) 0 and 1, so these just test the matrix part.
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(0, 0, 0), FloatVector3(0, 0, 0));

    auto r_xyz = vec3_from_xyz(sRGB->red_matrix_column());
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(255, 0, 0), r_xyz);

    auto g_xyz = vec3_from_xyz(sRGB->green_matrix_column());
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(0, 255, 0), g_xyz);

    auto b_xyz = vec3_from_xyz(sRGB->blue_matrix_column());
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(0, 0, 255), b_xyz);

    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(255, 255, 0), r_xyz + g_xyz);
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(255, 0, 255), r_xyz + b_xyz);
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(0, 255, 255), g_xyz + b_xyz);

    // FIXME: This should also be equal to sRGB->pcs_illuminant() and to the profiles mediaWhitePointTag,
    // but at the moment it's off by a bit too much. See also FIXME in WellKnownProfiles.cpp.
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(255, 255, 255), r_xyz + g_xyz + b_xyz);

    // These test the curve part.
    float f64 = sRGB_curve.evaluate(64 / 255.f);
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(64, 64, 64), (r_xyz + g_xyz + b_xyz) * f64);

    float f128 = sRGB_curve.evaluate(128 / 255.f);
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(128, 128, 128), (r_xyz + g_xyz + b_xyz) * f128);

    // Test for curve and matrix combined.
    float f192 = sRGB_curve.evaluate(192 / 255.f);
    EXPECT_APPROXIMATE_VECTOR3(xyz_from_sRGB(64, 128, 192), r_xyz * f64 + g_xyz * f128 + b_xyz * f192);
}

TEST_CASE(from_pcs)
{
    auto sRGB = MUST(Gfx::ICC::sRGB());

    auto sRGB_curve_pointer = MUST(Gfx::ICC::sRGB_curve());
    VERIFY(sRGB_curve_pointer->type() == Gfx::ICC::ParametricCurveTagData::Type);
    auto const& sRGB_curve = static_cast<Gfx::ICC::ParametricCurveTagData const&>(*sRGB_curve_pointer);

    auto sRGB_from_xyz = [&sRGB](FloatVector3 const& XYZ) {
        u8 rgb[3];
        // The first parameter, the source profile, is used to check if the PCS data is XYZ or LAB,
        // and what the source whitepoint is. We just need any profile with an XYZ PCS space,
        // so passing sRGB as source profile too is fine.
        MUST(sRGB->from_pcs(sRGB, XYZ, rgb));
        return Color(rgb[0], rgb[1], rgb[2]);
    };

    auto vec3_from_xyz = [](Gfx::ICC::XYZ const& xyz) {
        return FloatVector3 { xyz.X, xyz.Y, xyz.Z };
    };

    // At 0 and 255, the gamma curve is (exactly) 0 and 1, so these just test the matrix part.
    EXPECT_EQ(sRGB_from_xyz(FloatVector3 { 0, 0, 0 }), Color(0, 0, 0));

    auto r_xyz = vec3_from_xyz(sRGB->red_matrix_column());
    EXPECT_EQ(sRGB_from_xyz(r_xyz), Color(255, 0, 0));

    auto g_xyz = vec3_from_xyz(sRGB->green_matrix_column());
    EXPECT_EQ(sRGB_from_xyz(g_xyz), Color(0, 255, 0));

    auto b_xyz = vec3_from_xyz(sRGB->blue_matrix_column());
    EXPECT_EQ(sRGB_from_xyz(b_xyz), Color(0, 0, 255));

    EXPECT_EQ(sRGB_from_xyz(r_xyz + g_xyz), Color(255, 255, 0));
    EXPECT_EQ(sRGB_from_xyz(r_xyz + b_xyz), Color(255, 0, 255));
    EXPECT_EQ(sRGB_from_xyz(g_xyz + b_xyz), Color(0, 255, 255));
    EXPECT_EQ(sRGB_from_xyz(r_xyz + g_xyz + b_xyz), Color(255, 255, 255));

    // Test the inverse curve transform.
    float f64 = sRGB_curve.evaluate(64 / 255.f);
    EXPECT_EQ(sRGB_from_xyz((r_xyz + g_xyz + b_xyz) * f64), Color(64, 64, 64));

    float f128 = sRGB_curve.evaluate(128 / 255.f);
    EXPECT_EQ(sRGB_from_xyz((r_xyz + g_xyz + b_xyz) * f128), Color(128, 128, 128));

    // Test for curve and matrix combined.
    float f192 = sRGB_curve.evaluate(192 / 255.f);
    EXPECT_EQ(sRGB_from_xyz(r_xyz * f64 + g_xyz * f128 + b_xyz * f192), Color(64, 128, 192));
}

TEST_CASE(to_lab)
{
    auto sRGB = MUST(Gfx::ICC::sRGB());
    auto lab_from_sRGB = [&sRGB](u8 r, u8 g, u8 b) {
        u8 rgb[3] = { r, g, b };
        return MUST(sRGB->to_lab(rgb));
    };

    // The `expected` numbers are from https://colorjs.io/notebook/ for this snippet of code:
    //     new Color("srgb", [0, 0, 0]).lab.toString();
    //
    //     new Color("srgb", [1, 0, 0]).lab.toString();
    //     new Color("srgb", [0, 1, 0]).lab.toString();
    //     new Color("srgb", [0, 0, 1]).lab.toString();
    //
    //     new Color("srgb", [1, 1, 0]).lab.toString();
    //     new Color("srgb", [1, 0, 1]).lab.toString();
    //     new Color("srgb", [0, 1, 1]).lab.toString();
    //
    //     new Color("srgb", [1, 1, 1]).lab.toString();

    Gfx::CIELAB expected[] = {
        { 0, 0, 0 },
        { 54.29054294696968, 80.80492033462421, 69.89098825896275 },
        { 87.81853633115202, -79.27108223854806, 80.99459785152247 },
        { 29.56829715344471, 68.28740665215547, -112.02971798617645 },
        { 97.60701009682253, -15.749846639252663, 93.39361164266089 },
        { 60.16894098715946, 93.53959546199253, -60.50080231921204 },
        { 90.66601315791455, -50.65651077286893, -14.961666625736525 },
        { 100.00000139649632, -0.000007807961277528364, 0.000006766250648659877 },
    };

    // We're off by more than the default EXPECT_APPROXIMATE() error, so use EXPECT_APPROXIMATE_WITH_ERROR().
    // The difference is not too bad: ranges for L*, a*, b* are [0, 100], [-125, 125], [-125, 125],
    // so this is an error of considerably less than 0.1 for u8 channels.
#define EXPECT_APPROXIMATE_LAB(l1, l2)                   \
    EXPECT_APPROXIMATE_WITH_ERROR((l1).L, (l2).L, 0.01); \
    EXPECT_APPROXIMATE_WITH_ERROR((l1).a, (l2).a, 0.03); \
    EXPECT_APPROXIMATE_WITH_ERROR((l1).b, (l2).b, 0.02);

    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(0, 0, 0), expected[0]);
    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(255, 0, 0), expected[1]);
    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(0, 255, 0), expected[2]);
    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(0, 0, 255), expected[3]);
    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(255, 255, 0), expected[4]);
    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(255, 0, 255), expected[5]);
    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(0, 255, 255), expected[6]);
    EXPECT_APPROXIMATE_LAB(lab_from_sRGB(255, 255, 255), expected[7]);
}

TEST_CASE(malformed_profile)
{
    Array test_inputs = {
        TEST_INPUT("icc/oss-fuzz-testcase-57426.icc"sv),
        TEST_INPUT("icc/oss-fuzz-testcase-59551.icc"sv),
        TEST_INPUT("icc/oss-fuzz-testcase-60281.icc"sv)
    };

    for (auto test_input : test_inputs) {
        auto file = MUST(Core::MappedFile::map(test_input));
        auto profile_or_error = Gfx::ICC::Profile::try_load_from_externally_owned_memory(file->bytes());
        EXPECT(profile_or_error.is_error());
    }
}
