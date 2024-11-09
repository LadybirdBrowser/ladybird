/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Endian.h>
#include <LibGfx/ICC/DistinctFourCC.h>
#include <LibGfx/ICC/Profile.h>
#include <LibGfx/ICC/TagTypes.h>
#include <math.h>

namespace Gfx::ICC {

// ICC V4, 4.2 dateTimeNumber
// "All the dateTimeNumber values in a profile shall be in Coordinated Universal Time [...]."
struct DateTimeNumber {
    BigEndian<u16> year;
    BigEndian<u16> month;
    BigEndian<u16> day;
    BigEndian<u16> hours;
    BigEndian<u16> minutes;
    BigEndian<u16> seconds;
};

// ICC V4, 4.6 s15Fixed16Number
using s15Fixed16Number = i32;

// ICC V4, 4.7 u16Fixed16Number
using u16Fixed16Number = u32;

// ICC V4, 4.14 XYZNumber
struct XYZNumber {
    BigEndian<s15Fixed16Number> X;
    BigEndian<s15Fixed16Number> Y;
    BigEndian<s15Fixed16Number> Z;

    XYZNumber() = default;

    XYZNumber(XYZ const& xyz)
        : X(round(xyz.X * 0x1'0000))
        , Y(round(xyz.Y * 0x1'0000))
        , Z(round(xyz.Z * 0x1'0000))
    {
    }

    operator XYZ() const
    {
        return XYZ { X / (float)0x1'0000, Y / (float)0x1'0000, Z / (float)0x1'0000 };
    }
};

// ICC V4, 7.2 Profile header
struct ICCHeader {
    BigEndian<u32> profile_size;
    BigEndian<PreferredCMMType> preferred_cmm_type;

    u8 profile_version_major;
    u8 profile_version_minor_bugfix;
    BigEndian<u16> profile_version_zero;

    BigEndian<DeviceClass> profile_device_class;
    BigEndian<ColorSpace> data_color_space;
    BigEndian<ColorSpace> profile_connection_space; // "PCS" in the spec.

    DateTimeNumber profile_creation_time;

    BigEndian<u32> profile_file_signature;
    BigEndian<PrimaryPlatform> primary_platform;

    BigEndian<u32> profile_flags;
    BigEndian<DeviceManufacturer> device_manufacturer;
    BigEndian<DeviceModel> device_model;
    BigEndian<u64> device_attributes;
    BigEndian<RenderingIntent> rendering_intent;

    XYZNumber pcs_illuminant;

    BigEndian<Creator> profile_creator;

    u8 profile_id[16];
    u8 reserved[28];
};
static_assert(AssertSize<ICCHeader, 128>());

// ICC v4, 7.2.9 Profile file signature field
// "The profile file signature field shall contain the value “acsp” (61637370h) as a profile file signature."
constexpr u32 ProfileFileSignature = 0x61637370;

// ICC V4, 7.3 Tag table, Table 24 - Tag table structure
struct TagTableEntry {
    BigEndian<TagSignature> tag_signature;
    BigEndian<u32> offset_to_beginning_of_tag_data_element;
    BigEndian<u32> size_of_tag_data_element;
};
static_assert(AssertSize<TagTableEntry, 12>());

// Common bits of ICC v4, Table 40 — lut16Type encoding and Table 44 — lut8Type encoding
struct LUTHeader {
    u8 number_of_input_channels;
    u8 number_of_output_channels;
    u8 number_of_clut_grid_points;
    u8 reserved_for_padding;
    BigEndian<s15Fixed16Number> e_parameters[9];
};
static_assert(AssertSize<LUTHeader, 40>());

// Common bits of ICC v4, Table 45 — lutAToBType encoding and Table 47 — lutBToAType encoding
struct AdvancedLUTHeader {
    u8 number_of_input_channels;
    u8 number_of_output_channels;
    BigEndian<u16> reserved_for_padding;
    BigEndian<u32> offset_to_b_curves;
    BigEndian<u32> offset_to_matrix;
    BigEndian<u32> offset_to_m_curves;
    BigEndian<u32> offset_to_clut;
    BigEndian<u32> offset_to_a_curves;
};
static_assert(AssertSize<AdvancedLUTHeader, 24>());

// ICC v4, Table 46 — lutAToBType CLUT encoding
// ICC v4, Table 48 — lutBToAType CLUT encoding
// (They're identical.)
struct CLUTHeader {
    u8 number_of_grid_points_in_dimension[16];
    u8 precision_of_data_elements; // 1 for u8 entries, 2 for u16 entries.
    u8 reserved_for_padding[3];
};
static_assert(AssertSize<CLUTHeader, 20>());

// Table 49 — measurementType structure
struct MeasurementHeader {
    BigEndian<MeasurementTagData::StandardObserver> standard_observer;
    XYZNumber tristimulus_value_for_measurement_backing;
    BigEndian<MeasurementTagData::MeasurementGeometry> measurement_geometry;
    BigEndian<u16Fixed16Number> measurement_flare;
    BigEndian<MeasurementTagData::StandardIlluminant> standard_illuminant;
};
static_assert(AssertSize<MeasurementHeader, 28>());

// ICC v4, 10.15 multiLocalizedUnicodeType
struct MultiLocalizedUnicodeRawRecord {
    BigEndian<u16> language_code;
    BigEndian<u16> country_code;
    BigEndian<u32> string_length_in_bytes;
    BigEndian<u32> string_offset_in_bytes;
};
static_assert(AssertSize<MultiLocalizedUnicodeRawRecord, 12>());

// Table 66 — namedColor2Type encoding
struct NamedColorHeader {
    BigEndian<u32> vendor_specific_flag;
    BigEndian<u32> count_of_named_colors;
    BigEndian<u32> number_of_device_coordinates_of_each_named_color;
    u8 prefix_for_each_color_name[32]; // null-terminated
    u8 suffix_for_each_color_name[32]; // null-terminated
};
static_assert(AssertSize<NamedColorHeader, 76>());

// Table 84 — viewingConditionsType encoding
struct ViewingConditionsHeader {
    XYZNumber unnormalized_ciexyz_values_for_illuminant; // "(in which Y is in cd/m2)"
    XYZNumber unnormalized_ciexyz_values_for_surround;   // "(in which Y is in cd/m2)"
    BigEndian<MeasurementTagData::StandardIlluminant> illuminant_type;
};
static_assert(AssertSize<ViewingConditionsHeader, 28>());

}
