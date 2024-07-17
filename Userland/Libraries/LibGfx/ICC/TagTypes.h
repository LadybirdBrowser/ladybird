/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedPoint.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/ICC/DistinctFourCC.h>
#include <LibGfx/ICC/Enums.h>
#include <LibGfx/Vector3.h>
#include <math.h>

namespace Gfx::ICC {

// Does one-dimensional linear interpolation over a lookup table.
// Takes a span of values an x in [0.0, 1.0].
// Finds the two values in the span closest to x (where x = 0.0 is the span's first element and x = 1.0 the span's last element),
// and linearly interpolates between those two values, assumes all values are the same amount apart.
template<class T>
float lerp_1d(ReadonlySpan<T> values, float x)
{
    size_t n = values.size() - 1;
    size_t i = min(static_cast<size_t>(x * n), n - 1);
    return mix(static_cast<float>(values[i]), static_cast<float>(values[i + 1]), x * n - i);
}

// Does multi-dimensional linear interpolation over a lookup table.
// `size(i)` should returns the number of samples in the i'th dimension.
// `sample()` gets a vector where 0 <= i'th coordinate < size(i) and should return the value of the look-up table at that position.
inline FloatVector3 lerp_nd(Function<unsigned(size_t)> size, Function<FloatVector3(ReadonlySpan<unsigned> const&)> sample, ReadonlySpan<float> x)
{
    unsigned left_index[x.size()];
    float factor[x.size()];
    for (size_t i = 0; i < x.size(); ++i) {
        unsigned n = size(i) - 1;
        float ec = x[i] * n;
        left_index[i] = min(static_cast<unsigned>(ec), n - 1);
        factor[i] = ec - left_index[i];
    }

    FloatVector3 sample_output {};
    // The i'th bit of mask indicates if the i'th coordinate is rounded up or down.
    unsigned coordinates[x.size()];
    ReadonlySpan<unsigned> coordinates_span { coordinates, x.size() };
    for (size_t mask = 0; mask < (1u << x.size()); ++mask) {
        float sample_weight = 1.0f;
        for (size_t i = 0; i < x.size(); ++i) {
            coordinates[i] = left_index[i] + ((mask >> i) & 1u);
            sample_weight *= ((mask >> i) & 1u) ? factor[i] : 1.0f - factor[i];
        }
        sample_output += sample(coordinates_span) * sample_weight;
    }

    return sample_output;
}

using S15Fixed16 = AK::FixedPoint<16, i32>;
using U16Fixed16 = AK::FixedPoint<16, u32>;

struct XYZ {
    float X { 0 };
    float Y { 0 };
    float Z { 0 };

    bool operator==(const XYZ&) const = default;
};

TagTypeSignature tag_type(ReadonlyBytes tag_bytes);

class TagData : public RefCounted<TagData> {
public:
    u32 offset() const { return m_offset; }
    u32 size() const { return m_size; }
    TagTypeSignature type() const { return m_type; }

    virtual ~TagData() = default;

protected:
    TagData(u32 offset, u32 size, TagTypeSignature type)
        : m_offset(offset)
        , m_size(size)
        , m_type(type)
    {
    }

private:
    u32 m_offset;
    u32 m_size;
    TagTypeSignature m_type;
};

class UnknownTagData : public TagData {
public:
    UnknownTagData(u32 offset, u32 size, TagTypeSignature type)
        : TagData(offset, size, type)
    {
    }
};

// ICC v4, 10.2 chromaticityType
class ChromaticityTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6368726D }; // 'chrm'

    static ErrorOr<NonnullRefPtr<ChromaticityTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    // ICC v4, Table 31 — Colorant and phosphor encoding
    enum class PhosphorOrColorantType : u16 {
        Unknown = 0,
        ITU_R_BT_709_2 = 1,
        SMPTE_RP145 = 2,
        EBU_Tech_3213_E = 3,
        P22 = 4,
        P3 = 5,
        ITU_R_BT_2020 = 6,
    };

    static StringView phosphor_or_colorant_type_name(PhosphorOrColorantType);

    struct xyCoordinate {
        U16Fixed16 x;
        U16Fixed16 y;
    };

    ChromaticityTagData(u32 offset, u32 size, PhosphorOrColorantType phosphor_or_colorant_type, Vector<xyCoordinate> xy_coordinates)
        : TagData(offset, size, Type)
        , m_phosphor_or_colorant_type(phosphor_or_colorant_type)
        , m_xy_coordinates(move(xy_coordinates))
    {
    }

    PhosphorOrColorantType phosphor_or_colorant_type() const { return m_phosphor_or_colorant_type; }
    Vector<xyCoordinate> xy_coordinates() const { return m_xy_coordinates; }

private:
    PhosphorOrColorantType m_phosphor_or_colorant_type;
    Vector<xyCoordinate> m_xy_coordinates;
};

// ICC v4, 10.3 cicpType
// "The cicpType specifies Coding-independent code points for video signal type identification."
// See presentations at https://www.color.org/events/HDR_experts.xalter for background.
class CicpTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x63696370 }; // 'cicp'

    static ErrorOr<NonnullRefPtr<CicpTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    CicpTagData(u32 offset, u32 size, u8 color_primaries, u8 transfer_characteristics, u8 matrix_coefficients, u8 video_full_range_flag)
        : TagData(offset, size, Type)
        , m_color_primaries(color_primaries)
        , m_transfer_characteristics(transfer_characteristics)
        , m_matrix_coefficients(matrix_coefficients)
        , m_video_full_range_flag(video_full_range_flag)
    {
    }

    // "The fields ColourPrimaries, TransferCharacteristics, MatrixCoefficients, and VideoFullRangeFlag shall be
    //  encoded as specified in Recommendation ITU-T H.273. Recommendation ITU-T H.273 (ISO/IEC 23091-2)
    //  provides detailed descriptions of the code values and their interpretation."
    u8 color_primaries() const { return m_color_primaries; }
    u8 transfer_characteristics() const { return m_transfer_characteristics; }
    u8 matrix_coefficients() const { return m_matrix_coefficients; }
    u8 video_full_range_flag() const { return m_video_full_range_flag; }

private:
    u8 m_color_primaries;
    u8 m_transfer_characteristics;
    u8 m_matrix_coefficients;
    u8 m_video_full_range_flag;
};

// ICC v4, 10.6 curveType
class CurveTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x63757276 }; // 'curv'

    static ErrorOr<NonnullRefPtr<CurveTagData>> from_bytes(ReadonlyBytes, u32 offset);
    static ErrorOr<NonnullRefPtr<CurveTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    CurveTagData(u32 offset, u32 size, Vector<u16> values)
        : TagData(offset, size, Type)
        , m_values(move(values))
    {
    }

    // "The curveType embodies a one-dimensional function which maps an input value in the domain of the function
    //  to an output value in the range of the function. The domain and range values are in the range of 0,0 to 1,0.
    //  - When n is equal to 0, an identity response is assumed.
    //  - When n is equal to 1, then the curve value shall be interpreted as a gamma value, encoded as a
    //    u8Fixed8Number. Gamma shall be interpreted as the exponent in the equation y = pow(x,γ) and not as an inverse.
    //  - When n is greater than 1, the curve values (which embody a sampled one-dimensional function) shall be
    //    defined as follows:
    //    - The first entry represents the input value 0,0, the last entry represents the input value 1,0, and intermediate
    //      entries are uniformly spaced using an increment of 1,0/(n-1). These entries are encoded as uInt16Numbers
    //      (i.e. the values represented by the entries, which are in the range 0,0 to 1,0 are encoded in the range 0 to
    //      65 535). Function values between the entries shall be obtained through linear interpolation."
    Vector<u16> const& values() const { return m_values; }

    // x must be in [0..1].
    float evaluate(float x) const
    {
        VERIFY(0.f <= x && x <= 1.f);

        if (values().is_empty())
            return x;

        if (values().size() == 1)
            return powf(x, values()[0] / (float)0x100);

        return lerp_1d(values().span(), x) / 65535.0f;
    }

    // y must be in [0..1].
    float evaluate_inverse(float y) const
    {
        VERIFY(0.f <= y && y <= 1.f);

        if (values().is_empty())
            return y;

        if (values().size() == 1)
            return powf(y, 1.f / (values()[0] / (float)0x100));

        // FIXME: Verify somewhere that:
        // * values() is non-decreasing
        // * values()[0] is 0, values()[values().size() - 1] is 65535

        // FIXME: Use binary search.
        size_t n = values().size() - 1;
        size_t i = 0;
        for (; i < n; ++i) {
            if (values()[i] / 65535.f <= y && y <= values()[i + 1] / 65535.f)
                break;
        }

        float x1 = i / (float)n;
        float y1 = values()[i] / 65535.f;
        float x2 = (i + 1) / (float)n;
        float y2 = values()[i + 1] / 65535.f;

        // Flat line segment?
        if (y1 == y2)
            return (x1 + x2) / 2;

        return (y - y1) / (y2 - y1) * (x2 - x1) + x1; // Same as `((y - y1) / (y2 - y1) + i) / (float)n`
    }

private:
    Vector<u16> m_values;
};

struct EMatrix3x3 {
    // A row-major 3x3 matrix:
    // [ e[0] e[1] e[2] ]
    // [ e[3] e[4] e[5] ] * v
    // ] e[6] e[7] e[8] ]
    S15Fixed16 e[9];

    S15Fixed16 const& operator[](unsigned i) const
    {
        VERIFY(i < array_size(e));
        return e[i];
    }
};

// ICC v4, 10.10 lut16Type
class Lut16TagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6D667432 }; // 'mft2'

    static ErrorOr<NonnullRefPtr<Lut16TagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    Lut16TagData(u32 offset, u32 size, EMatrix3x3 e,
        u8 number_of_input_channels, u8 number_of_output_channels, u8 number_of_clut_grid_points,
        u16 number_of_input_table_entries, u16 number_of_output_table_entries,
        Vector<u16> input_tables, Vector<u16> clut_values, Vector<u16> output_tables)
        : TagData(offset, size, Type)
        , m_e(e)
        , m_number_of_input_channels(number_of_input_channels)
        , m_number_of_output_channels(number_of_output_channels)
        , m_number_of_clut_grid_points(number_of_clut_grid_points)
        , m_number_of_input_table_entries(number_of_input_table_entries)
        , m_number_of_output_table_entries(number_of_output_table_entries)
        , m_input_tables(move(input_tables))
        , m_clut_values(move(clut_values))
        , m_output_tables(move(output_tables))
    {
        VERIFY(m_input_tables.size() == number_of_input_channels * number_of_input_table_entries);
        VERIFY(m_output_tables.size() == number_of_output_channels * number_of_output_table_entries);

        VERIFY(number_of_input_table_entries >= 2);
        VERIFY(number_of_input_table_entries <= 4096);
        VERIFY(number_of_output_table_entries >= 2);
        VERIFY(number_of_output_table_entries <= 4096);
    }

    EMatrix3x3 const& e_matrix() const { return m_e; }

    u8 number_of_input_channels() const { return m_number_of_input_channels; }
    u8 number_of_output_channels() const { return m_number_of_output_channels; }
    u8 number_of_clut_grid_points() const { return m_number_of_clut_grid_points; }

    u16 number_of_input_table_entries() const { return m_number_of_input_table_entries; }
    u16 number_of_output_table_entries() const { return m_number_of_output_table_entries; }

    Vector<u16> const& input_tables() const { return m_input_tables; }
    Vector<u16> const& clut_values() const { return m_clut_values; }
    Vector<u16> const& output_tables() const { return m_output_tables; }

    ErrorOr<FloatVector3> evaluate(ColorSpace input_space, ColorSpace connection_space, ReadonlyBytes) const;

private:
    EMatrix3x3 m_e;

    u8 m_number_of_input_channels;
    u8 m_number_of_output_channels;
    u8 m_number_of_clut_grid_points;

    u16 m_number_of_input_table_entries;
    u16 m_number_of_output_table_entries;

    Vector<u16> m_input_tables;
    Vector<u16> m_clut_values;
    Vector<u16> m_output_tables;
};

// ICC v4, 10.11 lut8Type
class Lut8TagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6D667431 }; // 'mft1'

    static ErrorOr<NonnullRefPtr<Lut8TagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    Lut8TagData(u32 offset, u32 size, EMatrix3x3 e,
        u8 number_of_input_channels, u8 number_of_output_channels, u8 number_of_clut_grid_points,
        u16 number_of_input_table_entries, u16 number_of_output_table_entries,
        Vector<u8> input_tables, Vector<u8> clut_values, Vector<u8> output_tables)
        : TagData(offset, size, Type)
        , m_e(e)
        , m_number_of_input_channels(number_of_input_channels)
        , m_number_of_output_channels(number_of_output_channels)
        , m_number_of_clut_grid_points(number_of_clut_grid_points)
        , m_number_of_input_table_entries(number_of_input_table_entries)
        , m_number_of_output_table_entries(number_of_output_table_entries)
        , m_input_tables(move(input_tables))
        , m_clut_values(move(clut_values))
        , m_output_tables(move(output_tables))
    {
        VERIFY(m_input_tables.size() == number_of_input_channels * number_of_input_table_entries);
        VERIFY(m_output_tables.size() == number_of_output_channels * number_of_output_table_entries);

        VERIFY(number_of_input_table_entries == 256);
        VERIFY(number_of_output_table_entries == 256);
    }

    EMatrix3x3 const& e_matrix() const { return m_e; }

    u8 number_of_input_channels() const { return m_number_of_input_channels; }
    u8 number_of_output_channels() const { return m_number_of_output_channels; }
    u8 number_of_clut_grid_points() const { return m_number_of_clut_grid_points; }

    u16 number_of_input_table_entries() const { return m_number_of_input_table_entries; }
    u16 number_of_output_table_entries() const { return m_number_of_output_table_entries; }

    Vector<u8> const& input_tables() const { return m_input_tables; }
    Vector<u8> const& clut_values() const { return m_clut_values; }
    Vector<u8> const& output_tables() const { return m_output_tables; }

    ErrorOr<FloatVector3> evaluate(ColorSpace input_space, ColorSpace connection_space, ReadonlyBytes) const;

private:
    EMatrix3x3 m_e;

    u8 m_number_of_input_channels;
    u8 m_number_of_output_channels;
    u8 m_number_of_clut_grid_points;

    u16 m_number_of_input_table_entries;
    u16 m_number_of_output_table_entries;

    Vector<u8> m_input_tables;
    Vector<u8> m_clut_values;
    Vector<u8> m_output_tables;
};

struct EMatrix3x4 {
    // A row-major 3x3 matrix followed by a translation vector:
    // [ e[0] e[1] e[2] ]       [ e[9]  ]
    // [ e[3] e[4] e[5] ] * v + [ e[10] ]
    // [ e[6] e[7] e[8] ]       [ e[11] ]
    S15Fixed16 e[12];

    S15Fixed16 const& operator[](unsigned i) const
    {
        VERIFY(i < array_size(e));
        return e[i];
    }
};

struct CLUTData {
    Vector<u8, 4> number_of_grid_points_in_dimension;
    Variant<Vector<u8>, Vector<u16>> values;
};

using LutCurveType = NonnullRefPtr<TagData>; // FIXME: Variant<CurveTagData, ParametricCurveTagData> instead?

bool are_valid_curves(Optional<Vector<LutCurveType>> const& curves);

// ICC v4, 10.12 lutAToBType
class LutAToBTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6D414220 }; // 'mAB '

    static ErrorOr<NonnullRefPtr<LutAToBTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    LutAToBTagData(u32 offset, u32 size, u8 number_of_input_channels, u8 number_of_output_channels,
        Optional<Vector<LutCurveType>> a_curves, Optional<CLUTData> clut, Optional<Vector<LutCurveType>> m_curves, Optional<EMatrix3x4> e, Vector<LutCurveType> b_curves)
        : TagData(offset, size, Type)
        , m_number_of_input_channels(number_of_input_channels)
        , m_number_of_output_channels(number_of_output_channels)
        , m_a_curves(move(a_curves))
        , m_clut(move(clut))
        , m_m_curves(move(m_curves))
        , m_e(e)
        , m_b_curves(move(b_curves))
    {
        VERIFY(!m_a_curves.has_value() || m_a_curves->size() == m_number_of_input_channels);
        VERIFY(!m_m_curves.has_value() || m_m_curves->size() == m_number_of_output_channels);
        VERIFY(m_b_curves.size() == m_number_of_output_channels);

        VERIFY(number_of_input_channels == number_of_output_channels || m_clut.has_value());
        VERIFY(m_a_curves.has_value() == m_clut.has_value());
        VERIFY(m_m_curves.has_value() == m_e.has_value());

        VERIFY(are_valid_curves(m_a_curves));
        VERIFY(are_valid_curves(m_m_curves));
        VERIFY(are_valid_curves(m_b_curves));
    }

    u8 number_of_input_channels() const { return m_number_of_input_channels; }
    u8 number_of_output_channels() const { return m_number_of_output_channels; }

    Optional<Vector<LutCurveType>> const& a_curves() const { return m_a_curves; }
    Optional<CLUTData> const& clut() const { return m_clut; }
    Optional<Vector<LutCurveType>> const& m_curves() const { return m_m_curves; }
    Optional<EMatrix3x4> const& e_matrix() const { return m_e; }
    Vector<LutCurveType> const& b_curves() const { return m_b_curves; }

    // Returns the result of the LUT pipeline for u8 inputs.
    ErrorOr<FloatVector3> evaluate(ColorSpace connection_space, ReadonlyBytes) const;

private:
    u8 m_number_of_input_channels;
    u8 m_number_of_output_channels;

    // "It is possible to use any or all of these processing elements. At least one processing element shall be included.
    //  Only the following combinations are permitted:
    //  - B;
    //  - M, Matrix, B;
    //  - A, CLUT, B;
    //  - A, CLUT, M, Matrix, B."
    // This seems to imply that the B curve is not in fact optional.
    Optional<Vector<LutCurveType>> m_a_curves;
    Optional<CLUTData> m_clut;
    Optional<Vector<LutCurveType>> m_m_curves;
    Optional<EMatrix3x4> m_e;
    Vector<LutCurveType> m_b_curves;
};

// ICC v4, 10.13 lutBToAType
class LutBToATagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6D424120 }; // 'mBA '

    static ErrorOr<NonnullRefPtr<LutBToATagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    LutBToATagData(u32 offset, u32 size, u8 number_of_input_channels, u8 number_of_output_channels,
        Vector<LutCurveType> b_curves, Optional<EMatrix3x4> e, Optional<Vector<LutCurveType>> m_curves, Optional<CLUTData> clut, Optional<Vector<LutCurveType>> a_curves)
        : TagData(offset, size, Type)
        , m_number_of_input_channels(number_of_input_channels)
        , m_number_of_output_channels(number_of_output_channels)
        , m_b_curves(move(b_curves))
        , m_e(e)
        , m_m_curves(move(m_curves))
        , m_clut(move(clut))
        , m_a_curves(move(a_curves))
    {
        VERIFY(m_b_curves.size() == m_number_of_input_channels);
        VERIFY(!m_m_curves.has_value() || m_m_curves->size() == m_number_of_input_channels);
        VERIFY(!m_a_curves.has_value() || m_a_curves->size() == m_number_of_output_channels);

        VERIFY(m_e.has_value() == m_m_curves.has_value());
        VERIFY(m_clut.has_value() == m_a_curves.has_value());
        VERIFY(number_of_input_channels == number_of_output_channels || m_clut.has_value());

        VERIFY(are_valid_curves(m_b_curves));
        VERIFY(are_valid_curves(m_m_curves));
        VERIFY(are_valid_curves(m_a_curves));
    }

    u8 number_of_input_channels() const { return m_number_of_input_channels; }
    u8 number_of_output_channels() const { return m_number_of_output_channels; }

    Vector<LutCurveType> const& b_curves() const { return m_b_curves; }
    Optional<EMatrix3x4> const& e_matrix() const { return m_e; }
    Optional<Vector<LutCurveType>> const& m_curves() const { return m_m_curves; }
    Optional<CLUTData> const& clut() const { return m_clut; }
    Optional<Vector<LutCurveType>> const& a_curves() const { return m_a_curves; }

    // Returns the result of the LUT pipeline for u8 outputs.
    ErrorOr<void> evaluate(ColorSpace connection_space, FloatVector3 const&, Bytes) const;

private:
    u8 m_number_of_input_channels;
    u8 m_number_of_output_channels;

    // "It is possible to use any or all of these processing elements. At least one processing element shall be included.
    //  Only the following combinations are permitted:
    //  - B;
    //  - B, Matrix, M;
    //  - B, CLUT, A;
    //  - B, Matrix, M, CLUT, A."
    // This seems to imply that the B curve is not in fact optional.
    Vector<LutCurveType> m_b_curves;
    Optional<EMatrix3x4> m_e;
    Optional<Vector<LutCurveType>> m_m_curves;
    Optional<CLUTData> m_clut;
    Optional<Vector<LutCurveType>> m_a_curves;
};

// ICC v4, 10.14 measurementType
class MeasurementTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6D656173 }; // 'meas'

    static ErrorOr<NonnullRefPtr<MeasurementTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    // Table 50 — Standard observer encodings
    enum class StandardObserver {
        Unknown = 0,
        CIE_1931_standard_colorimetric_observer = 1,
        CIE_1964_standard_colorimetric_observer = 2,
    };
    static ErrorOr<void> validate_standard_observer(StandardObserver);
    static StringView standard_observer_name(StandardObserver);

    // Table 51 — Measurement geometry encodings
    enum class MeasurementGeometry {
        Unknown = 0,
        Degrees_0_45_or_45_0 = 1,
        Degrees_0_d_or_d_0 = 2,
    };
    static ErrorOr<void> validate_measurement_geometry(MeasurementGeometry);
    static StringView measurement_geometry_name(MeasurementGeometry);

    // Table 53 — Standard illuminant encodings
    enum class StandardIlluminant {
        Unknown = 0,
        D50 = 1,
        D65 = 2,
        D93 = 3,
        F2 = 4,
        D55 = 5,
        A = 6,
        Equi_Power_E = 7,
        F8 = 8,
    };
    static ErrorOr<void> validate_standard_illuminant(StandardIlluminant);
    static StringView standard_illuminant_name(StandardIlluminant);

    MeasurementTagData(u32 offset, u32 size, StandardObserver standard_observer, XYZ tristimulus_value_for_measurement_backing,
        MeasurementGeometry measurement_geometry, U16Fixed16 measurement_flare, StandardIlluminant standard_illuminant)
        : TagData(offset, size, Type)
        , m_standard_observer(standard_observer)
        , m_tristimulus_value_for_measurement_backing(tristimulus_value_for_measurement_backing)
        , m_measurement_geometry(measurement_geometry)
        , m_measurement_flare(measurement_flare)
        , m_standard_illuminant(standard_illuminant)
    {
    }

    StandardObserver standard_observer() const { return m_standard_observer; }
    XYZ const& tristimulus_value_for_measurement_backing() const { return m_tristimulus_value_for_measurement_backing; }
    MeasurementGeometry measurement_geometry() const { return m_measurement_geometry; }
    U16Fixed16 measurement_flare() const { return m_measurement_flare; }
    StandardIlluminant standard_illuminant() const { return m_standard_illuminant; }

private:
    StandardObserver m_standard_observer;
    XYZ m_tristimulus_value_for_measurement_backing;
    MeasurementGeometry m_measurement_geometry;
    U16Fixed16 m_measurement_flare;
    StandardIlluminant m_standard_illuminant;
};

// ICC v4, 10.15 multiLocalizedUnicodeType
class MultiLocalizedUnicodeTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6D6C7563 }; // 'mluc'

    static ErrorOr<NonnullRefPtr<MultiLocalizedUnicodeTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    struct Record {
        u16 iso_639_1_language_code;
        u16 iso_3166_1_country_code;
        String text;
    };

    MultiLocalizedUnicodeTagData(u32 offset, u32 size, Vector<Record> records)
        : TagData(offset, size, Type)
        , m_records(move(records))
    {
    }

    Vector<Record> const& records() const { return m_records; }

private:
    Vector<Record> m_records;
};

// ICC v4, 10.17 namedColor2Type
class NamedColor2TagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x6E636C32 }; // 'ncl2'

    static ErrorOr<NonnullRefPtr<NamedColor2TagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    // "The encoding is the same as the encodings for the PCS colour spaces
    //  as described in 6.3.4.2 and 10.8. Only PCSXYZ and
    //  legacy 16-bit PCSLAB encodings are permitted. PCS
    //  values shall be relative colorimetric."
    // (Which I suppose implies this type must not be used in DeviceLink profiles unless
    // the device's PCS happens to be PCSXYZ or PCSLAB.)
    struct XYZOrLAB {
        union {
            struct {
                u16 x, y, z;
            } xyz;
            struct {
                u16 L, a, b;
            } lab;
        };
    };

    NamedColor2TagData(u32 offset, u32 size, u32 vendor_specific_flag, u32 number_of_device_coordinates, String prefix, String suffix,
        Vector<String> root_names, Vector<XYZOrLAB> pcs_coordinates, Vector<u16> device_coordinates)
        : TagData(offset, size, Type)
        , m_vendor_specific_flag(vendor_specific_flag)
        , m_number_of_device_coordinates(number_of_device_coordinates)
        , m_prefix(move(prefix))
        , m_suffix(move(suffix))
        , m_root_names(move(root_names))
        , m_pcs_coordinates(move(pcs_coordinates))
        , m_device_coordinates(move(device_coordinates))
    {
        VERIFY(root_names.size() == pcs_coordinates.size());
        VERIFY(root_names.size() * number_of_device_coordinates == device_coordinates.size());

        for (u8 byte : m_prefix.bytes())
            VERIFY(byte < 128);
        VERIFY(m_prefix.bytes().size() < 32);

        for (u8 byte : m_suffix.bytes())
            VERIFY(byte < 128);
        VERIFY(m_suffix.bytes().size() < 32);

        for (auto const& root_name : m_root_names) {
            for (u8 byte : root_name.bytes())
                VERIFY(byte < 128);
            VERIFY(root_name.bytes().size() < 32);
        }
    }

    // "(least-significant 16 bits reserved for ICC use)"
    u32 vendor_specific_flag() const { return m_vendor_specific_flag; }

    // "If this field is 0, device coordinates are not provided."
    u32 number_of_device_coordinates() const { return m_number_of_device_coordinates; }

    u32 size() const { return m_root_names.size(); }

    // "In order to maintain maximum portability, it is strongly recommended that
    //  special characters of the 7-bit ASCII set not be used."
    String const& prefix() const { return m_prefix; }                        // "7-bit ASCII"
    String const& suffix() const { return m_suffix; }                        // "7-bit ASCII"
    String const& root_name(u32 index) const { return m_root_names[index]; } // "7-bit ASCII"

    // Returns 7-bit ASCII.
    ErrorOr<String> color_name(u32 index) const;

    // "The PCS representation corresponds to the header’s PCS field."
    XYZOrLAB const& pcs_coordinates(u32 index) const { return m_pcs_coordinates[index]; }

    // "The device representation corresponds to the header’s “data colour space” field."
    u16 const* device_coordinates(u32 index) const
    {
        VERIFY((index + 1) * m_number_of_device_coordinates <= m_device_coordinates.size());
        return m_device_coordinates.data() + index * m_number_of_device_coordinates;
    }

private:
    u32 m_vendor_specific_flag;
    u32 m_number_of_device_coordinates;
    String m_prefix;
    String m_suffix;
    Vector<String> m_root_names;
    Vector<XYZOrLAB> m_pcs_coordinates;
    Vector<u16> m_device_coordinates;
};

// ICC v4, 10.18 parametricCurveType
class ParametricCurveTagData : public TagData {
public:
    // Table 68 — parametricCurveType function type encoding
    enum class FunctionType {
        // Y = X**g
        Type0,

        // Y = (a*X + b)**g       if X >= -b/a
        //   = 0                  else
        Type1,
        CIE_122_1966 = Type1,

        // Y = (a*X + b)**g + c   if X >= -b/a
        //   = c                  else
        Type2,
        IEC_61966_1 = Type2,

        // Y = (a*X + b)**g       if X >= d
        //   =  c*X               else
        Type3,
        IEC_61966_2_1 = Type3,
        sRGB = Type3,

        // Y = (a*X + b)**g + e   if X >= d
        //   =  c*X + f           else
        Type4,
    };

    // "The domain and range of each function shall be [0,0 1,0]. Any function value outside the range shall be clipped
    //  to the range of the function."
    // "NOTE 1 The parameters selected for a parametric curve can result in complex or undefined values for the input range
    //  used. This can occur, for example, if d < -b/a. In such cases the behaviour of the curve is undefined."

    static constexpr TagTypeSignature Type { 0x70617261 }; // 'para'

    static ErrorOr<NonnullRefPtr<ParametricCurveTagData>> from_bytes(ReadonlyBytes, u32 offset);
    static ErrorOr<NonnullRefPtr<ParametricCurveTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    ParametricCurveTagData(u32 offset, u32 size, FunctionType function_type, Array<S15Fixed16, 7> parameters)
        : TagData(offset, size, Type)
        , m_function_type(function_type)
        , m_parameters(move(parameters))
    {
    }

    FunctionType function_type() const { return m_function_type; }

    static unsigned parameter_count(FunctionType);

    unsigned parameter_count() const { return parameter_count(function_type()); }
    S15Fixed16 parameter(size_t i) const
    {
        VERIFY(i < parameter_count());
        return m_parameters[i];
    }

    S15Fixed16 g() const { return m_parameters[0]; }
    S15Fixed16 a() const
    {
        VERIFY(function_type() >= FunctionType::Type1);
        return m_parameters[1];
    }
    S15Fixed16 b() const
    {
        VERIFY(function_type() >= FunctionType::Type1);
        return m_parameters[2];
    }
    S15Fixed16 c() const
    {
        VERIFY(function_type() >= FunctionType::Type2);
        return m_parameters[3];
    }
    S15Fixed16 d() const
    {
        VERIFY(function_type() >= FunctionType::Type3);
        return m_parameters[4];
    }
    S15Fixed16 e() const
    {
        VERIFY(function_type() >= FunctionType::Type4);
        return m_parameters[5];
    }
    S15Fixed16 f() const
    {
        VERIFY(function_type() >= FunctionType::Type4);
        return m_parameters[6];
    }

    // x must be in [0..1].
    float evaluate(float x) const
    {
        VERIFY(0.f <= x && x <= 1.f);

        switch (function_type()) {
        case FunctionType::Type0:
            return powf(x, (float)g());
        case FunctionType::Type1:
            if (x >= -(float)b() / (float)a())
                return powf((float)a() * x + (float)b(), (float)g());
            return 0;
        case FunctionType::Type2:
            if (x >= -(float)b() / (float)a())
                return powf((float)a() * x + (float)b(), (float)g()) + (float)c();
            return (float)c();
        case FunctionType::Type3:
            if (x >= (float)d())
                return powf((float)a() * x + (float)b(), (float)g());
            return (float)c() * x;
        case FunctionType::Type4:
            if (x >= (float)d())
                return powf((float)a() * x + (float)b(), (float)g()) + (float)e();
            return (float)c() * x + (float)f();
        }
        VERIFY_NOT_REACHED();
    }

    // y must be in [0..1].
    float evaluate_inverse(float y) const
    {
        VERIFY(0.f <= y && y <= 1.f);

        // See "Recommendations" section in https://www.color.org/whitepapers/ICC_White_Paper35-Use_of_the_parametricCurveType.pdf
        // Requirements for the curve to be non-decreasing:
        // * γ > 0
        // * a > 0 for types 1-4
        // * c ≥ 0 for types 3 and 4
        //
        // Types 3 and 4 additionally require:
        // To prevent negative discontinuities:
        // * cd ≤ (ad + b) for type 3
        // * cd + f ≤ (ad + b)^γ + e for type 4
        // To prevent complex numbers:
        // * ad + b ≥ 0
        // FIXME: Check these requirements somewhere.

        switch (function_type()) {
        case FunctionType::Type0:
            return powf(y, 1.f / (float)g());
        case FunctionType::Type1:
            return (powf(y, 1.f / (float)g()) - (float)b()) / (float)a();
        case FunctionType::Type2:
            // Only defined for Y >= c, so I suppose this requires c <= 0 in practice (?).
            return (powf(y - (float)c(), 1.f / (float)g()) - (float)b()) / (float)a();
        case FunctionType::Type3:
            if (y >= (float)c() * (float)d())
                return (powf(y, 1.f / (float)g()) - (float)b()) / (float)a();
            return y / (float)c();
        case FunctionType::Type4:
            if (y >= (float)c() * (float)d())
                return (powf(y - (float)e(), 1.f / (float)g()) - (float)b()) / (float)a();
            return (y - (float)f()) / (float)c();
        }
        VERIFY_NOT_REACHED();
    }

private:
    FunctionType m_function_type;

    // Contains, in this order, g a b c d e f.
    // Not all FunctionTypes use all parameters.
    Array<S15Fixed16, 7> m_parameters;
};

// ICC v4, 10.22 s15Fixed16ArrayType
class S15Fixed16ArrayTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x73663332 }; // 'sf32'

    static ErrorOr<NonnullRefPtr<S15Fixed16ArrayTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    S15Fixed16ArrayTagData(u32 offset, u32 size, Vector<S15Fixed16, 9> values)
        : TagData(offset, size, Type)
        , m_values(move(values))
    {
    }

    Vector<S15Fixed16, 9> const& values() const { return m_values; }

private:
    Vector<S15Fixed16, 9> m_values;
};

// ICC v4, 10.23 signatureType
class SignatureTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x73696720 }; // 'sig '

    static ErrorOr<NonnullRefPtr<SignatureTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    SignatureTagData(u32 offset, u32 size, u32 signature)
        : TagData(offset, size, Type)
        , m_signature(signature)
    {
    }

    u32 signature() const { return m_signature; }

    static Optional<StringView> colorimetric_intent_image_state_signature_name(u32);
    static Optional<StringView> perceptual_rendering_intent_gamut_signature_name(u32);
    static Optional<StringView> saturation_rendering_intent_gamut_signature_name(u32);
    static Optional<StringView> technology_signature_name(u32);

    Optional<StringView> name_for_tag(TagSignature);

private:
    u32 m_signature;
};

// ICC v2, 6.5.17 textDescriptionType
class TextDescriptionTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x64657363 }; // 'desc'

    static ErrorOr<NonnullRefPtr<TextDescriptionTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    TextDescriptionTagData(u32 offset, u32 size, String ascii_description, u32 unicode_language_code, Optional<String> unicode_description, Optional<String> macintosh_description)
        : TagData(offset, size, Type)
        , m_ascii_description(move(ascii_description))
        , m_unicode_language_code(unicode_language_code)
        , m_unicode_description(move(unicode_description))
        , m_macintosh_description(move(macintosh_description))
    {
        for (u8 byte : m_ascii_description.bytes())
            VERIFY(byte < 128);
    }

    // Guaranteed to be 7-bit ASCII.
    String const& ascii_description() const { return m_ascii_description; }

    u32 unicode_language_code() const { return m_unicode_language_code; }
    Optional<String> const& unicode_description() const { return m_unicode_description; }

    Optional<String> const& macintosh_description() const { return m_macintosh_description; }

private:
    String m_ascii_description;

    u32 m_unicode_language_code { 0 };
    Optional<String> m_unicode_description;

    Optional<String> m_macintosh_description;
};

// ICC v4, 10.24 textType
class TextTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x74657874 }; // 'text'

    static ErrorOr<NonnullRefPtr<TextTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    TextTagData(u32 offset, u32 size, String text)
        : TagData(offset, size, Type)
        , m_text(move(text))
    {
        for (u8 byte : m_text.bytes())
            VERIFY(byte < 128);
    }

    // Guaranteed to be 7-bit ASCII.
    String const& text() const { return m_text; }

private:
    String m_text;
};

// ICC v4, 10.30 viewingConditionsType
class ViewingConditionsTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x76696577 }; // 'view'

    static ErrorOr<NonnullRefPtr<ViewingConditionsTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    ViewingConditionsTagData(u32 offset, u32 size, XYZ const& unnormalized_ciexyz_values_for_illuminant,
        XYZ const& unnormalized_ciexyz_values_for_surround, MeasurementTagData::StandardIlluminant illuminant_type)
        : TagData(offset, size, Type)
        , m_unnormalized_ciexyz_values_for_illuminant(unnormalized_ciexyz_values_for_illuminant)
        , m_unnormalized_ciexyz_values_for_surround(unnormalized_ciexyz_values_for_surround)
        , m_illuminant_type(illuminant_type)
    {
    }

    XYZ const& unnormalized_ciexyz_values_for_illuminant() const { return m_unnormalized_ciexyz_values_for_illuminant; }
    XYZ const& unnormalized_ciexyz_values_for_surround() const { return m_unnormalized_ciexyz_values_for_surround; }
    MeasurementTagData::StandardIlluminant illuminant_type() const { return m_illuminant_type; }

private:
    XYZ m_unnormalized_ciexyz_values_for_illuminant; // "(in which Y is in cd/m2)"
    XYZ m_unnormalized_ciexyz_values_for_surround;   // "(in which Y is in cd/m2)"
    MeasurementTagData::StandardIlluminant m_illuminant_type;
};

// ICC v4, 10.31 XYZType
class XYZTagData : public TagData {
public:
    static constexpr TagTypeSignature Type { 0x58595A20 }; // 'XYZ '

    static ErrorOr<NonnullRefPtr<XYZTagData>> from_bytes(ReadonlyBytes, u32 offset, u32 size);

    XYZTagData(u32 offset, u32 size, Vector<XYZ, 1> xyzs)
        : TagData(offset, size, Type)
        , m_xyzs(move(xyzs))
    {
    }

    Vector<XYZ, 1> const& xyzs() const { return m_xyzs; }

    XYZ const& xyz() const
    {
        VERIFY(m_xyzs.size() == 1);
        return m_xyzs[0];
    }

private:
    Vector<XYZ, 1> m_xyzs;
};

inline ErrorOr<FloatVector3> Lut16TagData::evaluate(ColorSpace input_space, ColorSpace connection_space, ReadonlyBytes color_u8) const
{
    // See comment at start of LutAToBTagData::evaluate() for the clipping flow.
    VERIFY(connection_space == ColorSpace::PCSXYZ || connection_space == ColorSpace::PCSLAB);
    VERIFY(number_of_input_channels() == color_u8.size());

    // FIXME: This will be wrong once Profile::from_pcs_b_to_a() calls this function too.
    VERIFY(number_of_output_channels() == 3);

    // ICC v4, 10.11 lut8Type
    // "Data is processed using these elements via the following sequence:
    //  (matrix) ⇨ (1d input tables) ⇨ (multi-dimensional lookup table, CLUT) ⇨ (1d output tables)"

    Vector<float, 4> color;
    for (u8 c : color_u8)
        color.append(c / 255.0f);

    // "3 x 3 matrix (which shall be the identity matrix unless the input colour space is PCSXYZ)"
    // In practice, it's usually RGB or CMYK.
    if (input_space == ColorSpace::PCSXYZ) {
        EMatrix3x3 const& e = m_e;
        color = Vector<float, 4> {
            (float)e[0] * color[0] + (float)e[1] * color[1] + (float)e[2] * color[2],
            (float)e[3] * color[0] + (float)e[4] * color[1] + (float)e[5] * color[2],
            (float)e[6] * color[0] + (float)e[7] * color[1] + (float)e[8] * color[2],
        };
    }

    // "The input tables are arrays of 16-bit unsigned values. Each input table consists of a minimum of two and a maximum of 4096 uInt16Number integers.
    //  Each input table entry is appropriately normalized to the range 0 to 65535.
    //  The inputTable is of size (InputChannels x inputTableEntries x 2) bytes.
    //  When stored in this tag, the one-dimensional lookup tables are packed one after another"
    for (size_t c = 0; c < color.size(); ++c)
        color[c] = lerp_1d(m_input_tables.span().slice(c * m_number_of_input_table_entries, m_number_of_input_table_entries), color[c]) / 65535.0f;

    // "The CLUT is organized as an i-dimensional array with a given number of grid points in each dimension,
    //  where i is the number of input channels (input tables) in the transform.
    //  The dimension corresponding to the first input channel varies least rapidly and
    //  the dimension corresponding to the last input channel varies most rapidly.
    //  Each grid point value is an o-byte array, where o is the number of output channels.
    //  The first sequential byte of the entry contains the function value for the first output function,
    //  the second sequential byte of the entry contains the function value for the second output function,
    //  and so on until all the output functions have been supplied."
    auto sample = [this](ReadonlySpan<unsigned> const& coordinates) {
        size_t stride = 3;
        size_t offset = 0;
        for (int i = coordinates.size() - 1; i >= 0; --i) {
            offset += coordinates[i] * stride;
            stride *= m_number_of_clut_grid_points;
        }
        return FloatVector3 { (float)m_clut_values[offset], (float)m_clut_values[offset + 1], (float)m_clut_values[offset + 2] };
    };
    auto size = [this](size_t) { return m_number_of_clut_grid_points; };
    FloatVector3 output_color = lerp_nd(move(size), move(sample), color) / 65535.0f;

    // "The output tables are arrays of 16-bit unsigned values. Each output table consists of a minimum of two and a maximum of 4096 uInt16Number integers.
    //  Each output table entry is appropriately normalized to the range 0 to 65535.
    //  The outputTable is of size (OutputChannels x outputTableEntries x 2) bytes.
    //  When stored in this tag, the one-dimensional lookup tables are packed one after another"
    for (u8 c = 0; c < 3; ++c)
        output_color[c] = lerp_1d(m_output_tables.span().slice(c * m_number_of_output_table_entries, m_number_of_output_table_entries), output_color[c]) / 65535.0f;

    if (connection_space == ColorSpace::PCSXYZ) {
        // Table 11 - PCSXYZ X, Y or Z encoding
        output_color *= 65535 / 32768.0f;
    } else {
        VERIFY(connection_space == ColorSpace::PCSLAB);

        // ICC v4, 10.10 lut16Type
        // Note: lut16Type does _not_ use the encoding in 6.3.4.2 General PCS encoding!

        // "To convert colour values from this tag's legacy 16-bit PCSLAB encoding to the 16-bit PCSLAB encoding defined in 6.3.4.2 (Tables 12 and 13),
        //  multiply all values with 65 535/65 280 (i.e. FFFFh/FF00h).
        //  Any colour values that are in the value range of legacy 16-bit PCSLAB encoding, but not in the more recent 16-bit PCSLAB encoding,
        //  shall be clipped on a per-component basis."
        output_color *= 65535.0f / 65280.0f;

        // Table 42 — Legacy PCSLAB L* encoding
        output_color[0] = clamp(output_color[0] * 100.0f, 0.0f, 100.0f);

        // Table 43 — Legacy PCSLAB a* or PCSLAB b* encoding
        output_color[1] = clamp(output_color[1] * 255.0f - 128.0f, -128.0f, 127.0f);
        output_color[2] = clamp(output_color[2] * 255.0f - 128.0f, -128.0f, 127.0f);
    }

    return output_color;
}

inline ErrorOr<FloatVector3> Lut8TagData::evaluate(ColorSpace input_space, ColorSpace connection_space, ReadonlyBytes color_u8) const
{
    // See comment at start of LutAToBTagData::evaluate() for the clipping flow.
    VERIFY(connection_space == ColorSpace::PCSXYZ || connection_space == ColorSpace::PCSLAB);
    VERIFY(number_of_input_channels() == color_u8.size());

    // FIXME: This will be wrong once Profile::from_pcs_b_to_a() calls this function too.
    VERIFY(number_of_output_channels() == 3);

    // ICC v4, 10.11 lut8Type
    // "Data is processed using these elements via the following sequence:
    //  (matrix) ⇨ (1d input tables) ⇨ (multi-dimensional lookup table, CLUT) ⇨ (1d output tables)"

    Vector<float, 4> color;
    for (u8 c : color_u8)
        color.append(c / 255.0f);

    // "3 x 3 matrix (which shall be the identity matrix unless the input colour space is PCSXYZ)"
    // In practice, it's usually RGB or CMYK.
    if (input_space == ColorSpace::PCSXYZ) {
        EMatrix3x3 const& e = m_e;
        color = Vector<float, 4> {
            (float)e[0] * color[0] + (float)e[1] * color[1] + (float)e[2] * color[2],
            (float)e[3] * color[0] + (float)e[4] * color[1] + (float)e[5] * color[2],
            (float)e[6] * color[0] + (float)e[7] * color[1] + (float)e[8] * color[2],
        };
    }

    // "The input tables are arrays of uInt8Number values. Each input table consists of 256 uInt8Number integers.
    //  Each input table entry is appropriately normalized to the range 0 to 255.
    //  The inputTable is of size (InputChannels x 256) bytes.
    //  When stored in this tag, the one-dimensional lookup tables are packed one after another"
    for (size_t c = 0; c < color.size(); ++c)
        color[c] = lerp_1d(m_input_tables.span().slice(c * 256, 256), color[c]) / 255.0f;

    // "The CLUT is organized as an i-dimensional array with a given number of grid points in each dimension,
    //  where i is the number of input channels (input tables) in the transform.
    //  The dimension corresponding to the first input channel varies least rapidly and
    //  the dimension corresponding to the last input channel varies most rapidly.
    //  Each grid point value is an o-byte array, where o is the number of output channels.
    //  The first sequential byte of the entry contains the function value for the first output function,
    //  the second sequential byte of the entry contains the function value for the second output function,
    //  and so on until all the output functions have been supplied."
    auto sample = [this](ReadonlySpan<unsigned> const& coordinates) {
        size_t stride = 3;
        size_t offset = 0;
        for (int i = coordinates.size() - 1; i >= 0; --i) {
            offset += coordinates[i] * stride;
            stride *= m_number_of_clut_grid_points;
        }
        return FloatVector3 { (float)m_clut_values[offset], (float)m_clut_values[offset + 1], (float)m_clut_values[offset + 2] };
    };
    auto size = [this](size_t) { return m_number_of_clut_grid_points; };
    FloatVector3 output_color = lerp_nd(move(size), move(sample), color) / 255.0f;

    // "The output tables are arrays of uInt8Number values. Each output table consists of 256 uInt8Number integers.
    //  Each output table entry is appropriately normalized to the range 0 to 255.
    //  The outputTable is of size (OutputChannels x 256) bytes.
    //  When stored in this tag, the one-dimensional lookup tables are packed one after another"
    for (u8 c = 0; c < 3; ++c)
        output_color[c] = lerp_1d(m_output_tables.span().slice(c * 256, 256), output_color[c]) / 255.0f;

    if (connection_space == ColorSpace::PCSXYZ) {
        // "An 8-bit PCSXYZ encoding has not been defined, so the interpretation of a lut8Type in a profile that uses PCSXYZ is implementation specific."
    } else {
        VERIFY(connection_space == ColorSpace::PCSLAB);

        // ICC v4, 6.3.4.2 General PCS encoding
        // Table 12 — PCSLAB L* encoding
        output_color[0] *= 100.0f;

        // Table 13 — PCSLAB a* or PCSLAB b* encoding
        output_color[1] = output_color[1] * 255.0f - 128.0f;
        output_color[2] = output_color[2] * 255.0f - 128.0f;
    }

    return output_color;
}

inline ErrorOr<FloatVector3> LutAToBTagData::evaluate(ColorSpace connection_space, ReadonlyBytes color_u8) const
{
    VERIFY(connection_space == ColorSpace::PCSXYZ || connection_space == ColorSpace::PCSLAB);
    VERIFY(number_of_input_channels() == color_u8.size());
    VERIFY(number_of_output_channels() == 3);

    // ICC v4, 10.12 lutAToBType
    // "Data are processed using these elements via the following sequence:
    //  (“A” curves) ⇨ (multi-dimensional lookup table, CLUT) ⇨ (“M” curves) ⇨ (matrix) ⇨ (“B” curves).

    // "The domain and range of the A and B curves and CLUT are defined to consist of all real numbers between 0,0 and 1,0 inclusive.
    //  The first entry is located at 0,0, the last entry at 1,0, and intermediate entries are uniformly spaced using an increment of 1,0/(m-1).
    //  For the A and B curves, m is the number of entries in the table. For the CLUT, m is the number of grid points along each dimension.
    //  Since the domain and range of the tables are 0,0 to 1,0 it is necessary to convert all device values and PCSLAB values to this numeric range.
    //  It shall be assumed that the maximum value in each case is set to 1,0 and the minimum value to 0,0 and all intermediate values are
    //  linearly scaled accordingly."
    // Scaling from the full range to 0..1 before a curve and then back after the curve only to scale to 0..1 again before the next curve is a no-op,
    // so we only scale back to the full range at the very end of this function.

    auto evaluate_curve = [](LutCurveType const& curve, float f) {
        VERIFY(curve->type() == CurveTagData::Type || curve->type() == ParametricCurveTagData::Type);
        if (curve->type() == CurveTagData::Type)
            return static_cast<CurveTagData const&>(*curve).evaluate(f);
        return static_cast<ParametricCurveTagData const&>(*curve).evaluate(f);
    };

    FloatVector3 color;

    VERIFY(m_a_curves.has_value() == m_clut.has_value());
    if (m_a_curves.has_value()) {
        Vector<float, 4> in_color;

        auto const& a_curves = m_a_curves.value();
        for (u8 c = 0; c < color_u8.size(); ++c)
            in_color.append(evaluate_curve(a_curves[c], color_u8[c] / 255.0f));

        auto const& clut = m_clut.value();
        auto sample1 = [&clut]<typename T>(Vector<T> const& data, ReadonlySpan<unsigned> const& coordinates) {
            size_t stride = 3;
            size_t offset = 0;
            for (int i = coordinates.size() - 1; i >= 0; --i) {
                offset += coordinates[i] * stride;
                stride *= clut.number_of_grid_points_in_dimension[i];
            }
            return FloatVector3 { (float)data[offset], (float)data[offset + 1], (float)data[offset + 2] };
        };
        auto sample = [&clut, &sample1](ReadonlySpan<unsigned> const& coordinates) {
            return clut.values.visit(
                [&](Vector<u8> const& v) { return sample1(v, coordinates) / 255.0f; },
                [&](Vector<u16> const& v) { return sample1(v, coordinates) / 65535.0f; });
        };
        auto size = [&clut](size_t i) { return clut.number_of_grid_points_in_dimension[i]; };
        color = lerp_nd(move(size), move(sample), in_color);
    } else {
        color = FloatVector3 { color_u8[0] / 255.f, color_u8[1] / 255.f, color_u8[2] / 255.f };
    }

    VERIFY(m_m_curves.has_value() == m_e.has_value());
    if (m_m_curves.has_value()) {
        auto const& m_curves = m_m_curves.value();
        color = FloatVector3 {
            evaluate_curve(m_curves[0], color[0]),
            evaluate_curve(m_curves[1], color[1]),
            evaluate_curve(m_curves[2], color[2])
        };

        // ICC v4, 10.12.5 Matrix
        // "The resultant values Y1, Y2 and Y3 shall be clipped to the range 0,0 to 1,0 and used as inputs to the “B” curves."
        EMatrix3x4 const& e = m_e.value();
        FloatVector3 new_color = {
            (float)e[0] * color[0] + (float)e[1] * color[1] + (float)e[2] * color[2] + (float)e[9],
            (float)e[3] * color[0] + (float)e[4] * color[1] + (float)e[5] * color[2] + (float)e[10],
            (float)e[6] * color[0] + (float)e[7] * color[1] + (float)e[8] * color[2] + (float)e[11],
        };
        color = new_color.clamped(0.f, 1.f);
    }

    FloatVector3 output_color {
        evaluate_curve(m_b_curves[0], color[0]),
        evaluate_curve(m_b_curves[1], color[1]),
        evaluate_curve(m_b_curves[2], color[2])
    };

    // ICC v4, 6.3.4.2 General PCS encoding
    if (connection_space == ColorSpace::PCSXYZ) {
        // Table 11 - PCSXYZ X, Y or Z encoding
        output_color *= 65535 / 32768.0f;
    } else {
        VERIFY(connection_space == ColorSpace::PCSLAB);
        // Table 12 — PCSLAB L* encoding
        output_color[0] *= 100.0f;

        // Table 13 — PCSLAB a* or PCSLAB b* encoding
        output_color[1] = output_color[1] * 255.0f - 128.0f;
        output_color[2] = output_color[2] * 255.0f - 128.0f;
    }
    return output_color;
}

inline ErrorOr<void> LutBToATagData::evaluate(ColorSpace connection_space, FloatVector3 const& in_color, Bytes out_bytes) const
{
    VERIFY(connection_space == ColorSpace::PCSXYZ || connection_space == ColorSpace::PCSLAB);
    VERIFY(number_of_input_channels() == 3);
    VERIFY(number_of_output_channels() == out_bytes.size());

    // ICC v4, 10.13 lutBToAType
    // "Data are processed using these elements via the following sequence:
    //  (“B” curves) ⇨ (matrix) ⇨ (“M” curves) ⇨ (multi-dimensional lookup table, CLUT) ⇨ (“A” curves)."

    // See comment at start of LutAToBTagData::evaluate() for the clipping flow.
    // This function generally is the same as LutAToBTagData::evaluate() upside down.

    auto evaluate_curve = [](LutCurveType const& curve, float f) {
        VERIFY(curve->type() == CurveTagData::Type || curve->type() == ParametricCurveTagData::Type);
        if (curve->type() == CurveTagData::Type)
            return static_cast<CurveTagData const&>(*curve).evaluate(f);
        return static_cast<ParametricCurveTagData const&>(*curve).evaluate(f);
    };

    FloatVector3 color;
    if (connection_space == ColorSpace::PCSXYZ) {
        color = in_color * 32768 / 65535.0f;
    } else {
        VERIFY(connection_space == ColorSpace::PCSLAB);
        color[0] = in_color[0] / 100.0f;
        color[1] = (in_color[1] + 128.0f) / 255.0f;
        color[2] = (in_color[2] + 128.0f) / 255.0f;
    }

    color = FloatVector3 {
        evaluate_curve(m_b_curves[0], color[0]),
        evaluate_curve(m_b_curves[1], color[1]),
        evaluate_curve(m_b_curves[2], color[2])
    };

    VERIFY(m_e.has_value() == m_m_curves.has_value());
    if (m_e.has_value()) {
        // ICC v4, 10.13.3 Matrix
        // "The resultant values Y1, Y2 and Y3 shall be clipped to the range 0,0 to 1,0 and used as inputs to the “M” curves."
        EMatrix3x4 const& e = m_e.value();
        FloatVector3 new_color = {
            (float)e[0] * color[0] + (float)e[1] * color[1] + (float)e[2] * color[2] + (float)e[9],
            (float)e[3] * color[0] + (float)e[4] * color[1] + (float)e[5] * color[2] + (float)e[10],
            (float)e[6] * color[0] + (float)e[7] * color[1] + (float)e[8] * color[2] + (float)e[11],
        };
        color = new_color.clamped(0.f, 1.f);

        auto const& m_curves = m_m_curves.value();
        color = FloatVector3 {
            evaluate_curve(m_curves[0], color[0]),
            evaluate_curve(m_curves[1], color[1]),
            evaluate_curve(m_curves[2], color[2])
        };
    }

    VERIFY(m_clut.has_value() == m_a_curves.has_value());
    if (m_clut.has_value()) {
        // FIXME
        return Error::from_string_literal("LutBToATagData::evaluate: Not yet implemented when CLUT present");
    } else {
        VERIFY(number_of_output_channels() == 3);
        out_bytes[0] = round_to<u8>(color[0] * 255.0f);
        out_bytes[1] = round_to<u8>(color[1] * 255.0f);
        out_bytes[2] = round_to<u8>(color[2] * 255.0f);
    }

    return {};
}

}

template<>
struct AK::Formatter<Gfx::ICC::XYZ> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::ICC::XYZ const& xyz)
    {
        return Formatter<FormatString>::format(builder, "X = {}, Y = {}, Z = {}"sv, xyz.X, xyz.Y, xyz.Z);
    }
};
