/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGPreserveAspectRatio.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPreserveAspectRatio);

static constexpr Array<StringView, 10> align_keywords {
    "none"sv,
    "xMinYMin"sv,
    "xMidYMin"sv,
    "xMaxYMin"sv,
    "xMinYMid"sv,
    "xMidYMid"sv,
    "xMaxYMid"sv,
    "xMinYMax"sv,
    "xMidYMax"sv,
    "xMaxYMax"sv,
};

static StringView meet_or_slice_keyword(u16 meet_or_slice)
{
    return meet_or_slice == SVGPreserveAspectRatio::SVG_MEETORSLICE_SLICE ? "slice"sv : "meet"sv;
}

GC::Ref<SVGPreserveAspectRatio> SVGPreserveAspectRatio::create(GC::Ref<SVGElement> element, ReadOnly read_only)
{
    return GC::Heap::the().allocate<SVGPreserveAspectRatio>(element, read_only);
}

SVGPreserveAspectRatio::SVGPreserveAspectRatio(GC::Ref<SVGElement> element, ReadOnly read_only)
    : m_element(element)
    , m_read_only(read_only)
{
}

SVGPreserveAspectRatio::~SVGPreserveAspectRatio() = default;

void SVGPreserveAspectRatio::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

PreserveAspectRatio SVGPreserveAspectRatio::value() const
{
    // NB: An attribute that is not present or invalid takes its initial value, which is "xMidYMid meet".
    return parse_preserve_aspect_ratio(m_element->get_attribute_value(AttributeNames::preserveAspectRatio)).value_or({});
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGPreserveAspectRatio__align
u16 SVGPreserveAspectRatio::align() const
{
    // 1. Let value reflect the base value of a ‘preserveAspectRatio’ attribute. value is the current non-animated value
    //    of the attribute (using the attribute's initial value if it is not present or invalid).
    auto value = this->value();

    // 2. Return the constant value as specified in the alignment constant table above for the alignment keyword in
    //    value.
    return to_underlying(value.align) + SVG_PRESERVEASPECTRATIO_NONE;
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGPreserveAspectRatio__align
WebIDL::ExceptionOr<void> SVGPreserveAspectRatio::set_align(u16 value)
{
    // 1. If the SVGPreserveAspectRatio is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify a read-only SVGPreserveAspectRatio"_utf16);

    // 2. If value is SVG_PRESERVEASPECTRATIO_UNKNOWN or does not have a corresponding entry in the alignment keyword
    //    table above, then throw a TypeError.
    if (value == SVG_PRESERVEASPECTRATIO_UNKNOWN || value > SVG_PRESERVEASPECTRATIO_XMAXYMAX)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid alignment value"_utf16 };

    // 3. Let string be the corresponding keyword in the alignment keyword table above for value.
    StringBuilder string;
    string.append(align_keywords[value - SVG_PRESERVEASPECTRATIO_NONE]);

    // 4. Append a single U+0020 SPACE character to string.
    string.append(' ');

    // 5. Let meet or slice be the value that would be returned from the meetOrSlice member on this
    //    SVGPreserveAspectRatio.
    auto meet_or_slice = this->meet_or_slice();

    // 6. Append to string the corresponding keyword in the meet-or-slice keyword table above for meet or slice.
    string.append(meet_or_slice_keyword(meet_or_slice));

    // 7. Set the reflected ‘preserveAspectRatio’ attribute to string.
    m_element->set_attribute_value(AttributeNames::preserveAspectRatio, Utf16String::from_utf8(string.string_view()));
    return {};
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGPreserveAspectRatio__meetOrSlice
u16 SVGPreserveAspectRatio::meet_or_slice() const
{
    // 1. Let value be a ‘preserveAspectRatio’ value that reflects the base value of a ‘preserveAspectRatio’ attribute
    //    value is the current non-animated value of the attribute.
    auto value = this->value();

    // 2. If the meet-or-slice value is not present in value, then return SVG_MEETORSLICE_MEET.
    // 3. Otherwise, the meet-or-slice value is present. Return the constant value as specified in the meet-or-slice
    //    constant table above for the meet-or-slice keyword in value.
    // NB: A missing meet-or-slice value parses as meet.
    return value.meet_or_slice == PreserveAspectRatio::MeetOrSlice::Slice ? SVG_MEETORSLICE_SLICE : SVG_MEETORSLICE_MEET;
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGPreserveAspectRatio__meetOrSlice
WebIDL::ExceptionOr<void> SVGPreserveAspectRatio::set_meet_or_slice(u16 value)
{
    // 1. If the SVGPreserveAspectRatio is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify a read-only SVGPreserveAspectRatio"_utf16);

    // 2. If value is SVG_MEETORSLICE_UNKNOWN or does not have a corresponding entry in the meet-or-slice keyword table
    //    above, then throw a TypeError.
    if (value == SVG_MEETORSLICE_UNKNOWN || value > SVG_MEETORSLICE_SLICE)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid meet-or-slice value"_utf16 };

    // 3. Let align be the value that would be returned from the align member on this SVGPreserveAspectRatio.
    auto align = this->align();

    // 4. Let string be the corresponding keyword in the alignment keyword table above for align.
    StringBuilder string;
    string.append(align_keywords[align - SVG_PRESERVEASPECTRATIO_NONE]);

    // 5. Append a single U+0020 SPACE character to string.
    string.append(' ');

    // 6. Append to string the corresponding keyword in the meet-or-slice keyword table above for value.
    string.append(meet_or_slice_keyword(value));

    // 7. Set the reflected ‘preserveAspectRatio’ attribute to string.
    m_element->set_attribute_value(AttributeNames::preserveAspectRatio, Utf16String::from_utf8(string.string_view()));
    return {};
}

}
