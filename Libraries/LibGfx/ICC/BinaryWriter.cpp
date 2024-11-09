/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibGfx/ICC/BinaryFormat.h>
#include <LibGfx/ICC/BinaryWriter.h>
#include <LibGfx/ICC/Profile.h>
#include <time.h>

#pragma GCC diagnostic ignored "-Warray-bounds"

namespace Gfx::ICC {

static ErrorOr<ByteBuffer> encode_chromaticity(ChromaticityTagData const& tag_data)
{
    // ICC v4, 10.2 chromaticityType
    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + 2 * sizeof(u16) + tag_data.xy_coordinates().size() * 2 * sizeof(u16Fixed16Number)));

    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(ChromaticityTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    *bit_cast<BigEndian<u16>*>(bytes.data() + 8) = tag_data.xy_coordinates().size();
    *bit_cast<BigEndian<u16>*>(bytes.data() + 10) = static_cast<u16>(tag_data.phosphor_or_colorant_type());

    auto* coordinates = bit_cast<BigEndian<u16Fixed16Number>*>(bytes.data() + 12);
    for (size_t i = 0; i < tag_data.xy_coordinates().size(); ++i) {
        coordinates[2 * i] = tag_data.xy_coordinates()[i].x.raw();
        coordinates[2 * i + 1] = tag_data.xy_coordinates()[i].y.raw();
    }

    return bytes;
}

static ErrorOr<ByteBuffer> encode_cipc(CicpTagData const& tag_data)
{
    // ICC v4, 10.3 cicpType
    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + 4));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(CicpTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;
    bytes.data()[8] = tag_data.color_primaries();
    bytes.data()[9] = tag_data.transfer_characteristics();
    bytes.data()[10] = tag_data.matrix_coefficients();
    bytes.data()[11] = tag_data.video_full_range_flag();
    return bytes;
}

static u32 curve_encoded_size(CurveTagData const& tag_data)
{
    return 3 * sizeof(u32) + tag_data.values().size() * sizeof(u16);
}

static void encode_curve_to(CurveTagData const& tag_data, Bytes bytes)
{
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(CurveTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;
    *bit_cast<BigEndian<u32>*>(bytes.data() + 8) = tag_data.values().size();

    auto* values = bit_cast<BigEndian<u16>*>(bytes.data() + 12);
    for (size_t i = 0; i < tag_data.values().size(); ++i)
        values[i] = tag_data.values()[i];
}

static ErrorOr<ByteBuffer> encode_curve(CurveTagData const& tag_data)
{
    // ICC v4, 10.6 curveType
    auto bytes = TRY(ByteBuffer::create_uninitialized(curve_encoded_size(tag_data)));
    encode_curve_to(tag_data, bytes.bytes());
    return bytes;
}

static ErrorOr<ByteBuffer> encode_lut_16(Lut16TagData const& tag_data)
{
    // ICC v4, 10.10 lut16Type
    u32 input_tables_size = tag_data.input_tables().size();
    u32 clut_values_size = tag_data.clut_values().size();
    u32 output_tables_size = tag_data.output_tables().size();

    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + sizeof(LUTHeader) + 2 * sizeof(u16) + sizeof(u16) * (input_tables_size + clut_values_size + output_tables_size)));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(Lut16TagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto& header = *bit_cast<LUTHeader*>(bytes.data() + 8);
    header.number_of_input_channels = tag_data.number_of_input_channels();
    header.number_of_output_channels = tag_data.number_of_output_channels();
    header.number_of_clut_grid_points = tag_data.number_of_clut_grid_points();
    header.reserved_for_padding = 0;
    for (int i = 0; i < 9; ++i)
        header.e_parameters[i] = tag_data.e_matrix().e[i].raw();

    *bit_cast<BigEndian<u16>*>(bytes.data() + 8 + sizeof(LUTHeader)) = tag_data.number_of_input_table_entries();
    *bit_cast<BigEndian<u16>*>(bytes.data() + 8 + sizeof(LUTHeader) + 2) = tag_data.number_of_output_table_entries();

    auto* values = bit_cast<BigEndian<u16>*>(bytes.data() + 8 + sizeof(LUTHeader) + 4);
    for (u16 input_value : tag_data.input_tables())
        *values++ = input_value;

    for (u16 clut_value : tag_data.clut_values())
        *values++ = clut_value;

    for (u16 output_value : tag_data.output_tables())
        *values++ = output_value;

    return bytes;
}

static ErrorOr<ByteBuffer> encode_lut_8(Lut8TagData const& tag_data)
{
    // ICC v4, 10.11 lut8Type
    u32 input_tables_size = tag_data.input_tables().size();
    u32 clut_values_size = tag_data.clut_values().size();
    u32 output_tables_size = tag_data.output_tables().size();

    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + sizeof(LUTHeader) + input_tables_size + clut_values_size + output_tables_size));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(Lut8TagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto& header = *bit_cast<LUTHeader*>(bytes.data() + 8);
    header.number_of_input_channels = tag_data.number_of_input_channels();
    header.number_of_output_channels = tag_data.number_of_output_channels();
    header.number_of_clut_grid_points = tag_data.number_of_clut_grid_points();
    header.reserved_for_padding = 0;
    for (int i = 0; i < 9; ++i)
        header.e_parameters[i] = tag_data.e_matrix().e[i].raw();

    u8* values = bytes.data() + 8 + sizeof(LUTHeader);
    memcpy(values, tag_data.input_tables().data(), input_tables_size);
    values += input_tables_size;

    memcpy(values, tag_data.clut_values().data(), clut_values_size);
    values += clut_values_size;

    memcpy(values, tag_data.output_tables().data(), output_tables_size);

    return bytes;
}

static u32 curve_encoded_size(CurveTagData const&);
static void encode_curve_to(CurveTagData const&, Bytes);
static u32 parametric_curve_encoded_size(ParametricCurveTagData const&);
static void encode_parametric_curve_to(ParametricCurveTagData const&, Bytes);

static u32 byte_size_of_curve(LutCurveType const& curve)
{
    VERIFY(curve->type() == Gfx::ICC::CurveTagData::Type || curve->type() == Gfx::ICC::ParametricCurveTagData::Type);
    if (curve->type() == Gfx::ICC::CurveTagData::Type)
        return curve_encoded_size(static_cast<CurveTagData const&>(*curve));
    return parametric_curve_encoded_size(static_cast<ParametricCurveTagData const&>(*curve));
}

static u32 byte_size_of_curves(Vector<LutCurveType> const& curves)
{
    u32 size = 0;
    for (auto const& curve : curves)
        size += align_up_to(byte_size_of_curve(curve), 4);
    return size;
}

static void write_curve(Bytes bytes, LutCurveType const& curve)
{
    VERIFY(curve->type() == Gfx::ICC::CurveTagData::Type || curve->type() == Gfx::ICC::ParametricCurveTagData::Type);
    if (curve->type() == Gfx::ICC::CurveTagData::Type)
        encode_curve_to(static_cast<CurveTagData const&>(*curve), bytes);
    if (curve->type() == Gfx::ICC::ParametricCurveTagData::Type)
        encode_parametric_curve_to(static_cast<ParametricCurveTagData const&>(*curve), bytes);
}

static void write_curves(Bytes bytes, Vector<LutCurveType> const& curves)
{
    u32 offset = 0;
    for (auto const& curve : curves) {
        u32 size = byte_size_of_curve(curve);
        write_curve(bytes.slice(offset, size), curve);
        offset += align_up_to(size, 4);
    }
}

static u32 byte_size_of_clut(CLUTData const& clut)
{
    u32 data_size = clut.values.visit(
        [](Vector<u8> const& v) { return v.size(); },
        [](Vector<u16> const& v) { return 2 * v.size(); });
    return align_up_to(sizeof(CLUTHeader) + data_size, 4);
}

static void write_clut(Bytes bytes, CLUTData const& clut)
{
    auto& clut_header = *bit_cast<CLUTHeader*>(bytes.data());
    memset(clut_header.number_of_grid_points_in_dimension, 0, sizeof(clut_header.number_of_grid_points_in_dimension));
    VERIFY(clut.number_of_grid_points_in_dimension.size() <= sizeof(clut_header.number_of_grid_points_in_dimension));
    for (size_t i = 0; i < clut.number_of_grid_points_in_dimension.size(); ++i)
        clut_header.number_of_grid_points_in_dimension[i] = clut.number_of_grid_points_in_dimension[i];

    clut_header.precision_of_data_elements = clut.values.visit(
        [](Vector<u8> const&) { return 1; },
        [](Vector<u16> const&) { return 2; });

    memset(clut_header.reserved_for_padding, 0, sizeof(clut_header.reserved_for_padding));

    clut.values.visit(
        [&bytes](Vector<u8> const& v) {
            memcpy(bytes.data() + sizeof(CLUTHeader), v.data(), v.size());
        },
        [&bytes](Vector<u16> const& v) {
            auto* raw_clut = bit_cast<BigEndian<u16>*>(bytes.data() + sizeof(CLUTHeader));
            for (size_t i = 0; i < v.size(); ++i)
                raw_clut[i] = v[i];
        });
}

static void write_matrix(Bytes bytes, EMatrix3x4 const& e_matrix)
{
    auto* raw_e = bit_cast<BigEndian<s15Fixed16Number>*>(bytes.data());
    for (int i = 0; i < 12; ++i)
        raw_e[i] = e_matrix.e[i].raw();
}

static ErrorOr<ByteBuffer> encode_lut_a_to_b(LutAToBTagData const& tag_data)
{
    // ICC v4, 10.12 lutAToBType
    u32 a_curves_size = tag_data.a_curves().map(byte_size_of_curves).value_or(0);
    u32 clut_size = tag_data.clut().map(byte_size_of_clut).value_or(0);
    u32 m_curves_size = tag_data.m_curves().map(byte_size_of_curves).value_or(0);
    u32 e_matrix_size = tag_data.e_matrix().has_value() ? 12 * sizeof(s15Fixed16Number) : 0;
    u32 b_curves_size = byte_size_of_curves(tag_data.b_curves());

    auto bytes = TRY(ByteBuffer::create_zeroed(2 * sizeof(u32) + sizeof(AdvancedLUTHeader) + a_curves_size + clut_size + m_curves_size + e_matrix_size + b_curves_size));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(LutAToBTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto& header = *bit_cast<AdvancedLUTHeader*>(bytes.data() + 8);
    header.number_of_input_channels = tag_data.number_of_input_channels();
    header.number_of_output_channels = tag_data.number_of_output_channels();
    header.reserved_for_padding = 0;
    header.offset_to_b_curves = 0;
    header.offset_to_matrix = 0;
    header.offset_to_m_curves = 0;
    header.offset_to_clut = 0;
    header.offset_to_a_curves = 0;

    u32 offset = 2 * sizeof(u32) + sizeof(AdvancedLUTHeader);
    auto advance = [&offset](BigEndian<u32>& header_slot, u32 size) {
        header_slot = offset;
        VERIFY(size % 4 == 0);
        offset += size;
    };

    if (auto const& a_curves = tag_data.a_curves(); a_curves.has_value()) {
        write_curves(bytes.bytes().slice(offset, a_curves_size), a_curves.value());
        advance(header.offset_to_a_curves, a_curves_size);
    }
    if (auto const& clut = tag_data.clut(); clut.has_value()) {
        write_clut(bytes.bytes().slice(offset, clut_size), clut.value());
        advance(header.offset_to_clut, clut_size);
    }
    if (auto const& m_curves = tag_data.m_curves(); m_curves.has_value()) {
        write_curves(bytes.bytes().slice(offset, m_curves_size), m_curves.value());
        advance(header.offset_to_m_curves, m_curves_size);
    }
    if (auto const& e_matrix = tag_data.e_matrix(); e_matrix.has_value()) {
        write_matrix(bytes.bytes().slice(offset, e_matrix_size), e_matrix.value());
        advance(header.offset_to_matrix, e_matrix_size);
    }
    write_curves(bytes.bytes().slice(offset, b_curves_size), tag_data.b_curves());
    advance(header.offset_to_b_curves, b_curves_size);

    return bytes;
}

static ErrorOr<ByteBuffer> encode_lut_b_to_a(LutBToATagData const& tag_data)
{
    // ICC v4, 10.13 lutBToAType
    u32 b_curves_size = byte_size_of_curves(tag_data.b_curves());
    u32 e_matrix_size = tag_data.e_matrix().has_value() ? 12 * sizeof(s15Fixed16Number) : 0;
    u32 m_curves_size = tag_data.m_curves().map(byte_size_of_curves).value_or(0);
    u32 clut_size = tag_data.clut().map(byte_size_of_clut).value_or(0);
    u32 a_curves_size = tag_data.a_curves().map(byte_size_of_curves).value_or(0);

    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + sizeof(AdvancedLUTHeader) + b_curves_size + e_matrix_size + m_curves_size + clut_size + a_curves_size));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(LutBToATagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto& header = *bit_cast<AdvancedLUTHeader*>(bytes.data() + 8);
    header.number_of_input_channels = tag_data.number_of_input_channels();
    header.number_of_output_channels = tag_data.number_of_output_channels();
    header.reserved_for_padding = 0;
    header.offset_to_b_curves = 0;
    header.offset_to_matrix = 0;
    header.offset_to_m_curves = 0;
    header.offset_to_clut = 0;
    header.offset_to_a_curves = 0;

    u32 offset = 2 * sizeof(u32) + sizeof(AdvancedLUTHeader);
    auto advance = [&offset](BigEndian<u32>& header_slot, u32 size) {
        header_slot = offset;
        VERIFY(size % 4 == 0);
        offset += size;
    };

    write_curves(bytes.bytes().slice(offset, b_curves_size), tag_data.b_curves());
    advance(header.offset_to_b_curves, b_curves_size);
    if (auto const& e_matrix = tag_data.e_matrix(); e_matrix.has_value()) {
        write_matrix(bytes.bytes().slice(offset, e_matrix_size), e_matrix.value());
        advance(header.offset_to_matrix, e_matrix_size);
    }
    if (auto const& m_curves = tag_data.m_curves(); m_curves.has_value()) {
        write_curves(bytes.bytes().slice(offset, m_curves_size), m_curves.value());
        advance(header.offset_to_m_curves, m_curves_size);
    }
    if (auto const& clut = tag_data.clut(); clut.has_value()) {
        write_clut(bytes.bytes().slice(offset, clut_size), clut.value());
        advance(header.offset_to_clut, clut_size);
    }
    if (auto const& a_curves = tag_data.a_curves(); a_curves.has_value()) {
        write_curves(bytes.bytes().slice(offset, a_curves_size), a_curves.value());
        advance(header.offset_to_a_curves, a_curves_size);
    }

    return bytes;
}

static ErrorOr<ByteBuffer> encode_measurement(MeasurementTagData const& tag_data)
{
    // ICC v4, 10.14 measurementType
    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + sizeof(MeasurementHeader)));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(MeasurementTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto& header = *bit_cast<MeasurementHeader*>(bytes.data() + 8);
    header.standard_observer = tag_data.standard_observer();
    header.tristimulus_value_for_measurement_backing = tag_data.tristimulus_value_for_measurement_backing();
    header.measurement_geometry = tag_data.measurement_geometry();
    header.measurement_flare = tag_data.measurement_flare().raw();
    header.standard_illuminant = tag_data.standard_illuminant();

    return bytes;
}

static ErrorOr<ByteBuffer> encode_multi_localized_unicode(MultiLocalizedUnicodeTagData const& tag_data)
{
    // ICC v4, 10.15 multiLocalizedUnicodeType
    // "The Unicode strings in storage should be encoded as 16-bit big-endian, UTF-16BE,
    //  and should not be NULL terminated."
    size_t number_of_records = tag_data.records().size();
    size_t header_and_record_size = 4 * sizeof(u32) + number_of_records * sizeof(MultiLocalizedUnicodeRawRecord);

    size_t number_of_codepoints = 0;
    Vector<Utf16Data> utf16_strings;
    TRY(utf16_strings.try_ensure_capacity(number_of_records));
    for (auto const& record : tag_data.records()) {
        TRY(utf16_strings.try_append(TRY(utf8_to_utf16(record.text))));
        number_of_codepoints += utf16_strings.last().size();
    }

    size_t string_table_size = number_of_codepoints * sizeof(u16);

    auto bytes = TRY(ByteBuffer::create_uninitialized(header_and_record_size + string_table_size));

    auto* header = bit_cast<BigEndian<u32>*>(bytes.data());
    header[0] = static_cast<u32>(MultiLocalizedUnicodeTagData::Type);
    header[1] = 0;
    header[2] = number_of_records;
    header[3] = sizeof(MultiLocalizedUnicodeRawRecord);

    size_t offset = header_and_record_size;
    auto* records = bit_cast<MultiLocalizedUnicodeRawRecord*>(bytes.data() + 16);
    for (size_t i = 0; i < number_of_records; ++i) {
        records[i].language_code = tag_data.records()[i].iso_639_1_language_code;
        records[i].country_code = tag_data.records()[i].iso_3166_1_country_code;
        records[i].string_length_in_bytes = utf16_strings[i].size() * sizeof(u16);
        records[i].string_offset_in_bytes = offset;
        offset += records[i].string_length_in_bytes;
    }

    auto* string_table = bit_cast<BigEndian<u16>*>(bytes.data() + header_and_record_size);
    for (auto const& utf16_string : utf16_strings) {
        for (size_t i = 0; i < utf16_string.size(); ++i)
            string_table[i] = utf16_string[i];
        string_table += utf16_string.size();
    }

    return bytes;
}

static ErrorOr<ByteBuffer> encode_named_color_2(NamedColor2TagData const& tag_data)
{
    // ICC v4, 10.17 namedColor2Type
    unsigned const record_byte_size = 32 + sizeof(u16) * (3 + tag_data.number_of_device_coordinates());

    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + sizeof(NamedColorHeader) + tag_data.size() * record_byte_size));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = (u32)NamedColor2TagData::Type;
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto& header = *bit_cast<NamedColorHeader*>(bytes.data() + 8);
    header.vendor_specific_flag = tag_data.vendor_specific_flag();
    header.count_of_named_colors = tag_data.size();
    header.number_of_device_coordinates_of_each_named_color = tag_data.number_of_device_coordinates();
    memset(header.prefix_for_each_color_name, 0, 32);
    memcpy(header.prefix_for_each_color_name, tag_data.prefix().bytes().data(), tag_data.prefix().bytes().size());
    memset(header.suffix_for_each_color_name, 0, 32);
    memcpy(header.suffix_for_each_color_name, tag_data.suffix().bytes().data(), tag_data.suffix().bytes().size());

    u8* record = bytes.data() + 8 + sizeof(NamedColorHeader);
    for (size_t i = 0; i < tag_data.size(); ++i) {
        memset(record, 0, 32);
        memcpy(record, tag_data.root_name(i).bytes().data(), tag_data.root_name(i).bytes().size());

        auto* components = bit_cast<BigEndian<u16>*>(record + 32);
        components[0] = tag_data.pcs_coordinates(i).xyz.x;
        components[1] = tag_data.pcs_coordinates(i).xyz.y;
        components[2] = tag_data.pcs_coordinates(i).xyz.z;
        for (size_t j = 0; j < tag_data.number_of_device_coordinates(); ++j)
            components[3 + j] = tag_data.device_coordinates(i)[j];

        record += record_byte_size;
    }

    return bytes;
}

static u32 parametric_curve_encoded_size(ParametricCurveTagData const& tag_data)
{
    return 2 * sizeof(u32) + 2 * sizeof(u16) + tag_data.parameter_count() * sizeof(s15Fixed16Number);
}

static void encode_parametric_curve_to(ParametricCurveTagData const& tag_data, Bytes bytes)
{
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(ParametricCurveTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    *bit_cast<BigEndian<u16>*>(bytes.data() + 8) = static_cast<u16>(tag_data.function_type());
    *bit_cast<BigEndian<u16>*>(bytes.data() + 10) = 0;

    auto* parameters = bit_cast<BigEndian<s15Fixed16Number>*>(bytes.data() + 12);
    for (size_t i = 0; i < tag_data.parameter_count(); ++i)
        parameters[i] = tag_data.parameter(i).raw();
}

static ErrorOr<ByteBuffer> encode_parametric_curve(ParametricCurveTagData const& tag_data)
{
    // ICC v4, 10.18 parametricCurveType
    auto bytes = TRY(ByteBuffer::create_uninitialized(parametric_curve_encoded_size(tag_data)));
    encode_parametric_curve_to(tag_data, bytes.bytes());
    return bytes;
}

static ErrorOr<ByteBuffer> encode_s15_fixed_array(S15Fixed16ArrayTagData const& tag_data)
{
    // ICC v4, 10.22 s15Fixed16ArrayType
    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + tag_data.values().size() * sizeof(s15Fixed16Number)));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(S15Fixed16ArrayTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto* values = bit_cast<BigEndian<s15Fixed16Number>*>(bytes.data() + 8);
    for (size_t i = 0; i < tag_data.values().size(); ++i)
        values[i] = tag_data.values()[i].raw();

    return bytes;
}

static ErrorOr<ByteBuffer> encode_signature(SignatureTagData const& tag_data)
{
    // ICC v4, 10.23 signatureType
    auto bytes = TRY(ByteBuffer::create_uninitialized(3 * sizeof(u32)));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(SignatureTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;
    *bit_cast<BigEndian<u32>*>(bytes.data() + 8) = tag_data.signature();
    return bytes;
}

static ErrorOr<ByteBuffer> encode_text_description(TextDescriptionTagData const& tag_data)
{
    // ICC v2, 6.5.17 textDescriptionType
    // All lengths include room for a trailing nul character.
    // See also the many comments in TextDescriptionTagData::from_bytes().
    u32 ascii_size = sizeof(u32) + tag_data.ascii_description().bytes().size() + 1;

    // FIXME: Include tag_data.unicode_description() if it's set.
    u32 unicode_size = 2 * sizeof(u32);

    // FIXME: Include tag_data.macintosh_description() if it's set.
    u32 macintosh_size = sizeof(u16) + sizeof(u8) + 67;

    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + ascii_size + unicode_size + macintosh_size));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(TextDescriptionTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    // ASCII
    *bit_cast<BigEndian<u32>*>(bytes.data() + 8) = tag_data.ascii_description().bytes().size() + 1;
    memcpy(bytes.data() + 12, tag_data.ascii_description().bytes().data(), tag_data.ascii_description().bytes().size());
    bytes.data()[12 + tag_data.ascii_description().bytes().size()] = '\0';

    // Unicode
    // "Because the Unicode language code and Unicode count immediately follow the ASCII description,
    //  their alignment is not correct when the ASCII count is not a multiple of four"
    // So we can't use BigEndian<u32> here.
    u8* cursor = bytes.data() + 8 + ascii_size;
    u32 unicode_language_code = 0; // FIXME: Set to tag_data.unicode_language_code() once this writes unicode data.
    cursor[0] = unicode_language_code >> 24;
    cursor[1] = (unicode_language_code >> 16) & 0xff;
    cursor[2] = (unicode_language_code >> 8) & 0xff;
    cursor[3] = unicode_language_code & 0xff;
    cursor += 4;

    // FIXME: Include tag_data.unicode_description() if it's set.
    u32 ucs2_count = 0; // FIXME: If tag_data.unicode_description() is set, set this to its length plus room for one nul character.
    cursor[0] = ucs2_count >> 24;
    cursor[1] = (ucs2_count >> 16) & 0xff;
    cursor[2] = (ucs2_count >> 8) & 0xff;
    cursor[3] = ucs2_count & 0xff;
    cursor += 4;

    // Macintosh scriptcode
    u16 scriptcode_code = 0; // MacRoman
    cursor[0] = (scriptcode_code >> 8) & 0xff;
    cursor[1] = scriptcode_code & 0xff;
    cursor += 2;

    u8 macintosh_description_length = 0; // FIXME: If tag_data.macintosh_description() is set, set this to tis length plus room for one nul character.
    cursor[0] = macintosh_description_length;
    cursor += 1;
    memset(cursor, 0, 67);

    return bytes;
}

static ErrorOr<ByteBuffer> encode_text(TextTagData const& tag_data)
{
    // ICC v4, 10.24 textType
    // "The textType is a simple text structure that contains a 7-bit ASCII text string. The length of the string is obtained
    //  by subtracting 8 from the element size portion of the tag itself. This string shall be terminated with a 00h byte."
    auto text_bytes = tag_data.text().bytes();
    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + text_bytes.size() + 1));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(TextTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;
    memcpy(bytes.data() + 8, text_bytes.data(), text_bytes.size());
    *(bytes.data() + 8 + text_bytes.size()) = '\0';
    return bytes;
}

static ErrorOr<ByteBuffer> encode_viewing_conditions(ViewingConditionsTagData const& tag_data)
{
    // ICC v4, 10.30 viewingConditionsType
    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + sizeof(ViewingConditionsHeader)));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(ViewingConditionsTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto& header = *bit_cast<ViewingConditionsHeader*>(bytes.data() + 8);
    header.unnormalized_ciexyz_values_for_illuminant = tag_data.unnormalized_ciexyz_values_for_illuminant();
    header.unnormalized_ciexyz_values_for_surround = tag_data.unnormalized_ciexyz_values_for_surround();
    header.illuminant_type = tag_data.illuminant_type();

    return bytes;
}

static ErrorOr<ByteBuffer> encode_xyz(XYZTagData const& tag_data)
{
    // ICC v4, 10.31 XYZType
    auto bytes = TRY(ByteBuffer::create_uninitialized(2 * sizeof(u32) + tag_data.xyzs().size() * sizeof(XYZNumber)));
    *bit_cast<BigEndian<u32>*>(bytes.data()) = static_cast<u32>(XYZTagData::Type);
    *bit_cast<BigEndian<u32>*>(bytes.data() + 4) = 0;

    auto* xyzs = bit_cast<XYZNumber*>(bytes.data() + 8);
    for (size_t i = 0; i < tag_data.xyzs().size(); ++i)
        xyzs[i] = tag_data.xyzs()[i];

    return bytes;
}

static ErrorOr<Optional<ByteBuffer>> encode_tag_data(TagData const& tag_data)
{
    switch (tag_data.type()) {
    case ChromaticityTagData::Type:
        return encode_chromaticity(static_cast<ChromaticityTagData const&>(tag_data));
    case CicpTagData::Type:
        return encode_cipc(static_cast<CicpTagData const&>(tag_data));
    case CurveTagData::Type:
        return encode_curve(static_cast<CurveTagData const&>(tag_data));
    case Lut16TagData::Type:
        return encode_lut_16(static_cast<Lut16TagData const&>(tag_data));
    case Lut8TagData::Type:
        return encode_lut_8(static_cast<Lut8TagData const&>(tag_data));
    case LutAToBTagData::Type:
        return encode_lut_a_to_b(static_cast<LutAToBTagData const&>(tag_data));
    case LutBToATagData::Type:
        return encode_lut_b_to_a(static_cast<LutBToATagData const&>(tag_data));
    case MeasurementTagData::Type:
        return encode_measurement(static_cast<MeasurementTagData const&>(tag_data));
    case MultiLocalizedUnicodeTagData::Type:
        return encode_multi_localized_unicode(static_cast<MultiLocalizedUnicodeTagData const&>(tag_data));
    case NamedColor2TagData::Type:
        return encode_named_color_2(static_cast<NamedColor2TagData const&>(tag_data));
    case ParametricCurveTagData::Type:
        return encode_parametric_curve(static_cast<ParametricCurveTagData const&>(tag_data));
    case S15Fixed16ArrayTagData::Type:
        return encode_s15_fixed_array(static_cast<S15Fixed16ArrayTagData const&>(tag_data));
    case SignatureTagData::Type:
        return encode_signature(static_cast<SignatureTagData const&>(tag_data));
    case TextDescriptionTagData::Type:
        return encode_text_description(static_cast<TextDescriptionTagData const&>(tag_data));
    case TextTagData::Type:
        return encode_text(static_cast<TextTagData const&>(tag_data));
    case ViewingConditionsTagData::Type:
        return encode_viewing_conditions(static_cast<ViewingConditionsTagData const&>(tag_data));
    case XYZTagData::Type:
        return encode_xyz(static_cast<XYZTagData const&>(tag_data));
    }

    return OptionalNone {};
}

static ErrorOr<Vector<ByteBuffer>> encode_tag_datas(Profile const& profile, HashMap<TagData*, size_t>& tag_data_map)
{
    Vector<ByteBuffer> tag_data_bytes;
    TRY(tag_data_bytes.try_ensure_capacity(profile.tag_count()));

    TRY(profile.try_for_each_tag([&](auto, auto tag_data) -> ErrorOr<void> {
        if (tag_data_map.contains(tag_data.ptr()))
            return {};

        auto encoded_tag_data = TRY(encode_tag_data(tag_data));
        if (!encoded_tag_data.has_value())
            return {};

        tag_data_bytes.append(encoded_tag_data.release_value());
        TRY(tag_data_map.try_set(tag_data.ptr(), tag_data_bytes.size() - 1));
        return {};
    }));
    return tag_data_bytes;
}

static ErrorOr<void> encode_tag_table(ByteBuffer& bytes, Profile const& profile, u32 number_of_serialized_tags, Vector<size_t> const& offsets,
    Vector<ByteBuffer> const& tag_data_bytes, HashMap<TagData*, size_t> const& tag_data_map)
{
    // ICC v4, 7.3 Tag table
    // ICC v4, 7.3.1 Overview
    VERIFY(bytes.size() >= sizeof(ICCHeader) + sizeof(u32) + number_of_serialized_tags * sizeof(TagTableEntry));

    *bit_cast<BigEndian<u32>*>(bytes.data() + sizeof(ICCHeader)) = number_of_serialized_tags;

    TagTableEntry* tag_table_entries = bit_cast<TagTableEntry*>(bytes.data() + sizeof(ICCHeader) + sizeof(u32));
    int i = 0;
    profile.for_each_tag([&](auto tag_signature, auto tag_data) {
        auto index = tag_data_map.get(tag_data.ptr());
        if (!index.has_value())
            return;

        tag_table_entries[i].tag_signature = tag_signature;

        tag_table_entries[i].offset_to_beginning_of_tag_data_element = offsets[index.value()];
        tag_table_entries[i].size_of_tag_data_element = tag_data_bytes[index.value()].size();
        ++i;
    });

    return {};
}

static ErrorOr<void> encode_header(ByteBuffer& bytes, Profile const& profile)
{
    VERIFY(bytes.size() >= sizeof(ICCHeader));
    auto& raw_header = *bit_cast<ICCHeader*>(bytes.data());

    raw_header.profile_size = bytes.size();
    raw_header.preferred_cmm_type = profile.preferred_cmm_type().value_or(PreferredCMMType { 0 });

    raw_header.profile_version_major = profile.version().major_version();
    raw_header.profile_version_minor_bugfix = profile.version().minor_and_bugfix_version();
    raw_header.profile_version_zero = 0;

    raw_header.profile_device_class = profile.device_class();
    raw_header.data_color_space = profile.data_color_space();
    raw_header.profile_connection_space = profile.connection_space();

    DateTime profile_timestamp = profile.creation_timestamp();
    raw_header.profile_creation_time.year = profile_timestamp.year;
    raw_header.profile_creation_time.month = profile_timestamp.month;
    raw_header.profile_creation_time.day = profile_timestamp.day;
    raw_header.profile_creation_time.hours = profile_timestamp.hours;
    raw_header.profile_creation_time.minutes = profile_timestamp.minutes;
    raw_header.profile_creation_time.seconds = profile_timestamp.seconds;

    raw_header.profile_file_signature = ProfileFileSignature;
    raw_header.primary_platform = profile.primary_platform().value_or(PrimaryPlatform { 0 });

    raw_header.profile_flags = profile.flags().bits();
    raw_header.device_manufacturer = profile.device_manufacturer().value_or(DeviceManufacturer { 0 });
    raw_header.device_model = profile.device_model().value_or(DeviceModel { 0 });
    raw_header.device_attributes = profile.device_attributes().bits();
    raw_header.rendering_intent = profile.rendering_intent();

    raw_header.pcs_illuminant = profile.pcs_illuminant();

    raw_header.profile_creator = profile.creator().value_or(Creator { 0 });

    memset(raw_header.reserved, 0, sizeof(raw_header.reserved));

    auto id = Profile::compute_id(bytes);
    static_assert(sizeof(id.data) == sizeof(raw_header.profile_id));
    memcpy(raw_header.profile_id, id.data, sizeof(id.data));

    return {};
}

ErrorOr<ByteBuffer> encode(Profile const& profile)
{
    // Valid profiles always have tags. Profile only represents valid profiles.
    VERIFY(profile.tag_count() > 0);

    HashMap<TagData*, size_t> tag_data_map;
    Vector<ByteBuffer> tag_data_bytes = TRY(encode_tag_datas(profile, tag_data_map));

    u32 number_of_serialized_tags = 0;
    profile.for_each_tag([&](auto tag_signature, auto tag_data) {
        if (!tag_data_map.contains(tag_data.ptr())) {
            dbgln("ICC serialization: dropping tag {} because it has unknown type {}", tag_signature, tag_data->type());
            return;
        }
        number_of_serialized_tags++;
    });

    size_t tag_table_size = sizeof(u32) + number_of_serialized_tags * sizeof(TagTableEntry);
    size_t offset = sizeof(ICCHeader) + tag_table_size;
    Vector<size_t> offsets;
    for (auto const& bytes : tag_data_bytes) {
        TRY(offsets.try_append(offset));
        offset += align_up_to(bytes.size(), 4);
    }

    // Include padding after last element. Use create_zeroed() to fill padding bytes with null bytes.
    // ICC v4, 7.1.2:
    // "c) all tagged element data, including the last, shall be padded by no more than three following pad bytes to
    //  reach a 4-byte boundary;
    //  d) all pad bytes shall be NULL (as defined in ISO/IEC 646, character 0/0).
    // NOTE 1 This implies that the length is required to be a multiple of four."
    auto bytes = TRY(ByteBuffer::create_zeroed(offset));

    for (size_t i = 0; i < tag_data_bytes.size(); ++i)
        memcpy(bytes.data() + offsets[i], tag_data_bytes[i].data(), tag_data_bytes[i].size());

    TRY(encode_tag_table(bytes, profile, number_of_serialized_tags, offsets, tag_data_bytes, tag_data_map));
    TRY(encode_header(bytes, profile));

    return bytes;
}

}
