#!/usr/bin/env python3

# Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import re
from enum import Enum
from collections import namedtuple
from pathlib import Path
from typing import Any, List, Type


class EnumWithExportName(Enum):
    @classmethod
    def export_name(cls) -> str:
        return cls.__name__


class TIFFType(EnumWithExportName):
    @classmethod
    def export_name(cls) -> str:
        return "Type"

    def __new__(cls, *args):
        obj = object.__new__(cls)
        obj._value_ = args[0]
        obj.size = args[1]
        return obj

    # First value is the underlying u16, second one is the size in bytes
    Byte = 1, 1
    ASCII = 2, 1
    UnsignedShort = 3, 2
    UnsignedLong = 4, 4
    UnsignedRational = 5, 8
    Undefined = 7, 1
    SignedLong = 9, 4
    SignedRational = 10, 8
    Float = 11, 4
    Double = 12, 8
    IFD = 13, 4
    UTF8 = 129, 1


class Predictor(EnumWithExportName):
    NoPrediction = 1
    HorizontalDifferencing = 2


class Compression(EnumWithExportName):
    NoCompression = 1
    CCITTRLE = 2
    Group3Fax = 3
    Group4Fax = 4
    LZW = 5
    OldJPEG = 6
    JPEG = 7
    AdobeDeflate = 8
    PackBits = 32773
    PixarDeflate = 32946  # This is the old (and deprecated) code for AdobeDeflate


class PhotometricInterpretation(EnumWithExportName):
    WhiteIsZero = 0
    BlackIsZero = 1
    RGB = 2
    RGBPalette = 3
    TransparencyMask = 4
    CMYK = 5
    YCbCr = 6
    CIELab = 8


class FillOrder(EnumWithExportName):
    LeftToRight = 1
    RightToLeft = 2


class Orientation(EnumWithExportName):
    Default = 1
    FlipHorizontally = 2
    Rotate180 = 3
    FlipVertically = 4
    Rotate90ClockwiseThenFlipHorizontally = 5
    Rotate90Clockwise = 6
    FlipHorizontallyThenRotate90Clockwise = 7
    Rotate90CounterClockwise = 8


class PlanarConfiguration(EnumWithExportName):
    Chunky = 1
    Planar = 2


class ResolutionUnit(EnumWithExportName):
    NoAbsolute = 1
    Inch = 2
    Centimeter = 3


class SampleFormat(EnumWithExportName):
    Unsigned = 1
    Signed = 2
    Float = 3
    Undefined = 4


class ExtraSample(EnumWithExportName):
    Unspecified = 0
    AssociatedAlpha = 1
    UnassociatedAlpha = 2


tag_fields = ['id', 'types', 'counts', 'default', 'name', 'associated_enum', 'is_required']

Tag = namedtuple(
    'Tag',
    field_names=tag_fields,
    defaults=(None,) * len(tag_fields)
)

# FIXME: Some tag have only a few allowed values, we should ensure that
known_tags: List[Tag] = [
    Tag('256', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [1], None, "ImageWidth", is_required=True),
    Tag('257', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [1], None, "ImageLength", is_required=True),
    Tag('258', [TIFFType.UnsignedShort], [], None, "BitsPerSample", is_required=False),
    Tag('259', [TIFFType.UnsignedShort], [1], None, "Compression", Compression, is_required=True),
    Tag('262', [TIFFType.UnsignedShort], [1], None, "PhotometricInterpretation",
        PhotometricInterpretation, is_required=True),
    Tag('266', [TIFFType.UnsignedShort], [1], FillOrder.LeftToRight, "FillOrder", FillOrder),
    Tag('271', [TIFFType.ASCII], [], None, "Make"),
    Tag('272', [TIFFType.ASCII], [], None, "Model"),
    Tag('273', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [], None, "StripOffsets", is_required=False),
    Tag('274', [TIFFType.UnsignedShort], [1], Orientation.Default, "Orientation", Orientation),
    Tag('277', [TIFFType.UnsignedShort], [1], None, "SamplesPerPixel", is_required=False),
    Tag('278', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [1], None, "RowsPerStrip", is_required=False),
    Tag('279', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [], None, "StripByteCounts", is_required=False),
    Tag('282', [TIFFType.UnsignedRational], [1], None, "XResolution"),
    Tag('283', [TIFFType.UnsignedRational], [1], None, "YResolution"),
    Tag('284', [TIFFType.UnsignedShort], [1], PlanarConfiguration.Chunky, "PlanarConfiguration", PlanarConfiguration),
    Tag('285', [TIFFType.ASCII], [], None, "PageName"),
    Tag('292', [TIFFType.UnsignedLong], [1], 0, "T4Options"),
    Tag('296', [TIFFType.UnsignedShort], [1], ResolutionUnit.Inch, "ResolutionUnit", ResolutionUnit),
    Tag('305', [TIFFType.ASCII], [], None, "Software"),
    Tag('306', [TIFFType.ASCII], [20], None, "DateTime"),
    Tag('315', [TIFFType.ASCII], [], None, "Artist"),
    Tag('317', [TIFFType.UnsignedShort], [1], Predictor.NoPrediction, "Predictor", Predictor),
    Tag('320', [TIFFType.UnsignedShort], [], None, "ColorMap"),
    Tag('322', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [1], None, "TileWidth"),
    Tag('323', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [1], None, "TileLength"),
    Tag('324', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [], None, "TileOffsets"),
    Tag('325', [TIFFType.UnsignedShort, TIFFType.UnsignedLong], [], None, "TileByteCounts"),
    Tag('338', [TIFFType.UnsignedShort], [], None, "ExtraSamples", ExtraSample),
    Tag('339', [TIFFType.UnsignedShort], [], SampleFormat.Unsigned, "SampleFormat", SampleFormat),
    Tag('34665', [TIFFType.UnsignedLong, TIFFType.IFD], [1], None, "ExifIFD"),
    Tag('34675', [TIFFType.Undefined], [], None, "ICCProfile"),
]

HANDLE_TAG_SIGNATURE_TEMPLATE = ("ErrorOr<void> {namespace}handle_tag(Function<ErrorOr<void>(u32)>&& subifd_handler, "
                                 "ExifMetadata& metadata, u16 tag, {namespace}Type type, u32 count, "
                                 "Vector<{namespace}Value>&& value)")
HANDLE_TAG_SIGNATURE = HANDLE_TAG_SIGNATURE_TEMPLATE.format(namespace="")
HANDLE_TAG_SIGNATURE_TIFF_NAMESPACE = HANDLE_TAG_SIGNATURE_TEMPLATE.format(namespace="TIFF::")

ENSURE_BASELINE_TAG_PRESENCE = "ErrorOr<void> ensure_baseline_tags_are_present(ExifMetadata const& metadata)"
TIFF_TYPE_FROM_U16 = "ErrorOr<Type> tiff_type_from_u16(u16 type)"
SIZE_OF_TIFF_TYPE = "u8 size_of_type(Type type)"

LICENSE = R"""/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */"""


def export_enum_to_cpp(e: Type[EnumWithExportName]) -> str:
    output = f'enum class {e.export_name()} {{\n'

    for entry in e:
        output += f'    {entry.name} = {entry.value},\n'

    output += "};\n"
    return output


def export_enum_to_string_converter(enums: List[Type[EnumWithExportName]]) -> str:
    stringifier_internals = []
    for e in enums:
        single_stringifier = fR"""    if constexpr (IsSame<E, {e.export_name()}>) {{
        switch (value) {{
            default:
                return "Invalid value for {e.export_name()}"sv;"""
        for entry in e:
            single_stringifier += fR"""
            case {e.export_name()}::{entry.name}:
                return "{entry.name}"sv;"""

        single_stringifier += R"""
        }
    }"""
        stringifier_internals.append(single_stringifier)

    stringifier_internals_str = '\n'.join(stringifier_internals)

    out = fR"""template<Enum E>
StringView name_for_enum_tag_value(E value) {{
{stringifier_internals_str}
    VERIFY_NOT_REACHED();
}}"""

    return out


def export_tag_related_enums(tags: List[Tag]) -> str:
    exported_enums = []
    for tag in tags:
        if tag.associated_enum:
            exported_enums.append(export_enum_to_cpp(tag.associated_enum))

    return '\n'.join(exported_enums)


def promote_type(t: TIFFType) -> TIFFType:
    if t == TIFFType.UnsignedShort:
        return TIFFType.UnsignedLong
    if t == TIFFType.Float:
        return TIFFType.Double
    return t


def tiff_type_to_cpp(t: TIFFType, with_promotion: bool = True) -> str:
    # To simplify the code generator and the ExifMetadata class API, all u16 are promoted to u32
    # Note that the Value<> type doesn't include u16 for this reason
    if with_promotion:
        t = promote_type(t)
    if t in [TIFFType.ASCII, TIFFType.UTF8]:
        return 'String'
    if t == TIFFType.Undefined:
        return 'ByteBuffer'
    if t == TIFFType.UnsignedShort:
        return 'u16'
    if t == TIFFType.UnsignedLong or t == TIFFType.IFD:
        return 'u32'
    if t == TIFFType.UnsignedRational:
        return 'TIFF::Rational<u32>'
    if t == TIFFType.Float:
        return 'float'
    if t == TIFFType.Double:
        return 'double'
    raise RuntimeError(f'Type "{t}" not recognized, please update tiff_type_to_read_only_cpp()')


def is_container(t: TIFFType) -> bool:
    """
        Some TIFF types are defined on the unit scale but are intended to be used within a collection.
        An example of that are ASCII strings defined as N * byte. Let's intercept that and generate
        a nice API instead of Vector<u8>.
    """
    return t in [TIFFType.ASCII, TIFFType.Byte, TIFFType.Undefined, TIFFType.UTF8]


def export_promoter() -> str:
    output = R"""template<typename T>
struct TypePromoter {
    using Type = T;
};
"""
    specialization_template = R"""template<>
struct TypePromoter<{}> {{
    using Type = {};
}};
"""
    for t in TIFFType:
        if promote_type(t) != t:
            output += specialization_template.format(tiff_type_to_cpp(t, with_promotion=False), tiff_type_to_cpp(t))

    return output


def retrieve_biggest_type(types: List[TIFFType]) -> TIFFType:
    return TIFFType(max([t.value for t in types]))


def pascal_case_to_snake_case(name: str) -> str:
    name = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', name)
    return re.sub('([a-z0-9])([A-Z])', r'\1_\2', name).lower()


def default_value_to_cpp(value: Any) -> str:
    if isinstance(value, EnumWithExportName):
        return f'TIFF::{value.export_name()}::{value.name}'
    return str(value)


def generate_getter(tag: Tag) -> str:
    biggest_type = retrieve_biggest_type(tag.types)
    variant_inner_type = tiff_type_to_cpp(biggest_type)

    extracted_value_template = f"(*possible_value)[{{}}].get<{variant_inner_type}>()"

    tag_final_type = variant_inner_type
    if tag.associated_enum:
        tag_final_type = f"TIFF::{tag.associated_enum.__name__}"
        extracted_value_template = f"static_cast<{tag_final_type}>({extracted_value_template})"

    single_count = len(tag.counts) == 1 and tag.counts[0] == 1 or is_container(biggest_type)
    if single_count:
        return_type = tag_final_type
        if is_container(biggest_type):
            return_type += ' const&'
        unpacked_if_needed = f"return {extracted_value_template.format(0)};"
    else:
        if len(tag.counts) == 1:
            container_type = f'Array<{tag_final_type}, {tag.counts[0]}>'
            container_initialization = f'{container_type} tmp{{}};'
        else:
            container_type = f'Vector<{tag_final_type}>'
            container_initialization = fR"""{container_type} tmp{{}};
        auto maybe_failure = tmp.try_resize(possible_value->size());
        if (maybe_failure.is_error())
            return OptionalNone {{}};
        """

        return_type = container_type
        unpacked_if_needed = fR"""
        {container_initialization}
        for (u32 i = 0; i < possible_value->size(); ++i)
            tmp[i] = {extracted_value_template.format('i')};

        return tmp;"""

    signature = fR"    Optional<{return_type}> {pascal_case_to_snake_case(tag.name)}() const"

    if tag.default is not None and single_count:
        return_if_empty = f'{default_value_to_cpp(tag.default)}'
    else:
        return_if_empty = 'OptionalNone {}'

    body = fR"""
    {{
        auto const& possible_value = m_data.get("{tag.name}"sv);
        if (!possible_value.has_value())
            return {return_if_empty};
        {unpacked_if_needed}
    }}
"""

    return signature + body


def generate_metadata_class(tags: List[Tag]) -> str:
    getters = '\n'.join([generate_getter(tag) for tag in tags])

    output = fR"""class ExifMetadata : public Metadata {{
public:
    virtual ~ExifMetadata() = default;

{getters}
private:
    friend {HANDLE_TAG_SIGNATURE_TIFF_NAMESPACE};

    virtual void fill_main_tags() const override {{
        if (model().has_value())
            m_main_tags.set("Model"sv, model().value());
        if (make().has_value())
            m_main_tags.set("Manufacturer"sv, make().value());
        if (software().has_value())
            m_main_tags.set("Software"sv, software().value());
        if (date_time().has_value())
            m_main_tags.set("Creation Time"sv, date_time().value());
        if (artist().has_value())
            m_main_tags.set("Author"sv, artist().value());
    }}

    void add_entry(StringView key, Vector<TIFF::Value>&& value) {{
        m_data.set(key, move(value));
    }}

    HashMap<StringView, Vector<TIFF::Value>> m_data;
}};
"""

    return output


def generate_metadata_file(tags: List[Tag]) -> str:
    output = fR"""{LICENSE}

#pragma once

#include <AK/HashMap.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>

namespace Gfx {{

class ExifMetadata;

namespace TIFF {{

{export_enum_to_cpp(TIFFType)}

template<OneOf<u32, i32> x32>
struct Rational {{
    using Type = x32;
    x32 numerator;
    x32 denominator;

    double as_double() const {{
        return static_cast<double>(numerator) / denominator;
    }}
}};

{export_promoter()}

// Note that u16 is not include on purpose
using Value = Variant<ByteBuffer, String, u32, Rational<u32>, i32, Rational<i32>, double>;

{export_tag_related_enums(known_tags)}

{export_enum_to_string_converter([tag.associated_enum for tag in known_tags if tag.associated_enum] + [TIFFType])}

{HANDLE_TAG_SIGNATURE};
{ENSURE_BASELINE_TAG_PRESENCE};
{TIFF_TYPE_FROM_U16};
{SIZE_OF_TIFF_TYPE};

}}

{generate_metadata_class(tags)}

}}

template<typename T>
struct AK::Formatter<Gfx::TIFF::Rational<T>> : Formatter<FormatString> {{
    ErrorOr<void> format(FormatBuilder& builder, Gfx::TIFF::Rational<T> value)
    {{
        return Formatter<FormatString>::format(builder, "{{}} ({{}}/{{}})"sv,
            value.as_double(), value.numerator, value.denominator);
    }}
}};

template<>
struct AK::Formatter<Gfx::TIFF::Value> : Formatter<FormatString> {{
    ErrorOr<void> format(FormatBuilder& builder, Gfx::TIFF::Value const& value)
    {{
        String content;
        value.visit(
            [&](ByteBuffer const& buffer) {{
                content = MUST(String::formatted("Buffer of size: {{}}"sv, buffer.size()));
            }},
            [&](auto const& other) {{
                content = MUST(String::formatted("{{}}", other));
            }}
        );

        return Formatter<FormatString>::format(builder, "{{}}"sv, content);
    }}
}};
"""
    return output


def generate_tag_handler(tag: Tag) -> str:
    not_in_type_list = f"({' && '.join([f'type != Type::{t.name}' for t in tag.types])})"

    not_in_count_list = ''
    if len(tag.counts) != 0:
        not_in_count_list = f"|| ({' && '.join([f'count != {c}' for c in tag.counts])})"
    pre_condition = fR"""if ({not_in_type_list}
            {not_in_count_list})
            return Error::from_string_literal("TIFFImageDecoderPlugin: Tag {tag.name} invalid");"""

    check_value = ''
    if tag.associated_enum is not None:
        not_in_value_list = f"({' && '.join([f'v != {v.value}' for v in tag.associated_enum])})"
        check_value = fR"""
        for (u32 i = 0; i < value.size(); ++i) {{
            TRY(value[i].visit(
                []({tiff_type_to_cpp(tag.types[0])} const& v) -> ErrorOr<void> {{
                    if ({not_in_value_list})
                        return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid value for tag {tag.name}");
                    return {{}};
                }},
                [&](auto const&) -> ErrorOr<void> {{
                    VERIFY_NOT_REACHED();
                }})
            );
        }}
"""

    handle_subifd = ''
    if TIFFType.IFD in tag.types:
        if tag.counts != [1]:
            raise RuntimeError("Accessing `value[0]` in the C++ code might fail!")
        handle_subifd = f'TRY(subifd_handler(value[0].get<{tiff_type_to_cpp(TIFFType.IFD)}>()));'

    output = fR"""    case {tag.id}:
        // {tag.name}

        dbgln_if(TIFF_DEBUG, "{tag.name}({{}}): {{}}", name_for_enum_tag_value(type), format_tiff_value(tag, value));

        {pre_condition}
        {check_value}
        {handle_subifd}
        metadata.add_entry("{tag.name}"sv, move(value));
        break;
"""

    return output


def generate_tag_handler_file(tags: List[Tag]) -> str:
    formatter_for_tag_with_enum = '\n'.join([fR"""        case {tag.id}:
            return MUST(String::from_utf8(
                name_for_enum_tag_value(static_cast<{tag.associated_enum.export_name()}>(v.get<u32>()))));"""
                                             for tag in tags if tag.associated_enum])

    ensure_tags_are_present = '\n'.join([fR"""    if (!metadata.{pascal_case_to_snake_case(tag.name)}().has_value())
        return Error::from_string_literal("Unable to decode image, missing required tag {tag.name}.");
""" for tag in filter(lambda tag: tag.is_required, known_tags)])

    tiff_type_from_u16_cases = '\n'.join([fR"""    case to_underlying(Type::{t.name}):
        return Type::{t.name};""" for t in TIFFType])

    size_of_tiff_type_cases = '\n'.join([fR"""    case Type::{t.name}:
        return {t.size};""" for t in TIFFType])

    output = fR"""{LICENSE}

#include <AK/Debug.h>
#include <AK/String.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>

namespace Gfx::TIFF {{

static String value_formatter(u32 tag_id, Value const& v) {{
    switch (tag_id) {{
{formatter_for_tag_with_enum}
        default:
            return MUST(String::formatted("{{}}", v));
    }}
}}

[[maybe_unused]] static String format_tiff_value(u32 tag_id, Vector<Value> const& values) {{
    if (values.size() == 1)
        return MUST(String::formatted("{{}}", value_formatter(tag_id, values[0])));

    StringBuilder builder;
    builder.append('[');

    for (u32 i = 0; i < values.size(); ++i) {{
        builder.appendff("{{}}", value_formatter(tag_id, values[i]));
        if (i != values.size() - 1)
            builder.append(", "sv);
    }}

    builder.append(']');
    return MUST(builder.to_string());
}}

{ENSURE_BASELINE_TAG_PRESENCE}
{{
{ensure_tags_are_present}
    return {{}};
}}

{TIFF_TYPE_FROM_U16}
{{
    switch (type) {{
{tiff_type_from_u16_cases}
    default:
        return Error::from_string_literal("TIFFImageDecoderPlugin: Unknown type");
    }}
}}

{SIZE_OF_TIFF_TYPE}
{{
    switch (type) {{
{size_of_tiff_type_cases}
    default:
        VERIFY_NOT_REACHED();
    }}
}}

{HANDLE_TAG_SIGNATURE}
{{
    switch (tag) {{
"""

    output += '\n'.join([generate_tag_handler(t) for t in tags])

    output += R"""
    default:
        dbgln_if(TIFF_DEBUG, "UnknownTag({}, {}): {}",
                tag, name_for_enum_tag_value(type), format_tiff_value(tag, value));
    }

    return {};
}

}
"""
    return output


def update_file(target: Path, new_content: str):
    should_update = True

    if target.exists():
        with target.open('r') as file:
            content = file.read()
            if content == new_content:
                should_update = False

    if should_update:
        with target.open('w') as file:
            file.write(new_content)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-o', '--output')
    args = parser.parse_args()

    output_path = Path(args.output)

    update_file(output_path / 'TIFFMetadata.h', generate_metadata_file(known_tags))
    update_file(output_path / 'TIFFTagHandler.cpp', generate_tag_handler_file(known_tags))


if __name__ == '__main__':
    main()
