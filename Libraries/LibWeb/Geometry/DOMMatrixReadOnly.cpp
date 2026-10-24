/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Geometry/DOMMatrixReadOnly.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Geometry {

extern "C" {

// Defined in Bindings/Rust/src/geometry/exports.rs.
void web_dom_matrix_read_only_elements(void const* matrix, Array<double, 16>* out_elements);
bool web_dom_matrix_read_only_is_2d(void const* matrix);
bool web_dom_matrix_read_only_to_string(void const* matrix, FlatPtr* out_string);

// What Bindings/Rust/src/geometry/dom_matrix_read_only.rs asks of LibWeb.
bool web_dom_matrix_parse_transform_list(FlatPtr transform_list, Array<double, 16>* out_elements, bool* out_is_2d);

// https://drafts.fxtf.org/geometry/#parse-a-string-into-an-abstract-matrix, which stores the elements m11, m12, m13,
// m14, m21, ..., m44 of the matrix, in that order, and 2dTransform, or returns false for failure.
bool web_dom_matrix_parse_transform_list(FlatPtr transform_list_raw, Array<double, 16>* out_elements, bool* out_is_2d)
{
    auto transform_list = Utf16String::from_raw(transform_list_raw).to_utf8();

    // 1. If transformList is the empty string, set it to the string "matrix(1, 0, 0, 1, 0, 0)".
    if (transform_list.is_empty())
        transform_list = "matrix(1, 0, 0, 1, 0, 0)"_string;

    // 2. Parse transformList into parsedValue given the grammar for the CSS transform property. The result will be a
    //    <transform-list>, the keyword none, or failure. If parsedValue is failure, or any <transform-function> has
    //    <length> values without absolute length units, or any keyword other than none is used, then return failure.
    //    [CSS3-SYNTAX] [CSS3-TRANSFORMS]
    CSS::Parser::ParsingParams parsing_params {};
    parsing_params.value_context = { CSS::Parser::SpecialContext::DOMMatrixInitString };
    auto transform_style_value = parse_css_value(parsing_params, transform_list, CSS::PropertyID::Transform);
    if (!transform_style_value || (transform_style_value->is_keyword() && transform_style_value->to_keyword() != CSS::Keyword::None) || transform_style_value->is_unresolved())
        return false;

    // 3. If parsedValue is none, set parsedValue to a <transform-list> containing a single identity matrix.
    // 4. Let 2dTransform track the 2D/3D dimension status of parsedValue.
    // 5. Transform all <transform-function>s to 4x4 abstract matrices by following the “Mathematical Description of
    //    Transform Functions”. [CSS3-TRANSFORMS]
    // 6. Let matrix be a 4x4 abstract matrix as shown in the initial figure of this section. Post-multiply all
    //    matrices from left to right and set matrix to this product.
    // NB: A <length> without an absolute unit is a failure as well, which Rust reports when it cannot resolve a function.
    Array<float, 16> matrix;
    bool is_2d_transform = true;
    if (!CSS::ComputedValuesFFI::rust_transform_list_to_abstract_matrix(transform_style_value->rust_style_value_data(), matrix.span(), &is_2d_transform))
        return false;

    // 7. Return matrix and 2dTransform.
    // NB: The matrix is row by row, so its element mRC is in row C and column R.
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column)
            (*out_elements)[(column * 4) + row] = matrix[(row * 4) + column];
    }
    *out_is_2d = is_2d_transform;
    return true;
}
}

Array<double, 16> DOMMatrixReadOnly::elements() const
{
    Array<double, 16> elements;
    web_dom_matrix_read_only_elements(this, &elements);
    return elements;
}

bool DOMMatrixReadOnly::is2d() const
{
    return web_dom_matrix_read_only_is_2d(this);
}

WebIDL::ExceptionOr<Utf16String> DOMMatrixReadOnly::to_string() const
{
    FlatPtr string = 0;
    if (!web_dom_matrix_read_only_to_string(this, &string))
        return WebIDL::InvalidStateError::create("Cannot stringify non-finite matrix values"_utf16);
    return Utf16String::adopt_raw(string);
}

}
