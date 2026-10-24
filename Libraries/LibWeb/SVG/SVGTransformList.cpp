/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTransformList);

GC::Ref<SVGTransformList> SVGTransformList::create(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadOnlyList read_only)
{
    return GC::Heap::the().allocate<SVGTransformList>(element, move(reflected_attribute), read_only);
}

SVGTransformList::SVGTransformList(GC::Ref<SVGElement> element, Utf16FlyString reflected_attribute, ReadOnlyList read_only)
    : SVGList(read_only)
    , m_element(element)
    , m_reflected_attribute(move(reflected_attribute))
{
    synchronize();
}

void SVGTransformList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
    visitor.visit(m_element);
}

void SVGTransformList::attach(GC::Ref<SVGTransform> const& transform)
{
    transform->attach(*this, read_only() == ReadOnlyList::Yes ? SVGTransform::ReadOnly::Yes : SVGTransform::ReadOnly::No);
}

void SVGTransformList::detach(GC::Ref<SVGTransform> const& transform)
{
    transform->detach();
}

GC::Ref<SVGTransform> SVGTransformList::copy_if_not_detached(GC::Ref<SVGTransform> transform)
{
    if (transform->is_detached())
        return transform;
    return SVGTransform::create(transform->value());
}

// https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
void SVGTransformList::synchronize()
{
    // NB: The list does not need to be synchronized with a value that it has just reserialized itself.
    if (m_is_reserializing)
        return;

    // 1. Let value be the base value of the reflected content attribute (using the attribute's initial value if it is
    //    not present or invalid).
    // NB: The initial value of the transform presentation attributes is none, which is also what an invalid value is
    //     treated as. A value of none has no transform functions.
    Vector<SVGTransform::Value> value;
    if (auto transforms = parse_transform(m_element->get_attribute_value(m_reflected_attribute)); transforms.has_value()) {
        value.ensure_capacity(transforms->size());
        for (auto const& transform : *transforms)
            value.unchecked_append(SVGTransform::Value::from_transform(transform));
    }

    synchronize_items(value, [] { return SVGTransform::create(); }, [](GC::Ref<SVGTransform> const& item, SVGTransform::Value const& transform) { item->set_value_from_reflected_attribute(transform); });
}

// https://svgwg.org/svg2-draft/types.html#TermReserialize
void SVGTransformList::reserialize_reflected_attribute()
{
    // 1. Let elements be the list of values in value.
    // 2. Let serialized elements be a list of strings, where each string is formed based on the corresponding value in
    //    elements and its type:
    //    a SVGTransform object: The string is the serialization of the SVGTransform object's matrix object.
    // 3. Set the content attribute to a string consisting of the strings in serialized elements joined and separated by
    //    single U+0020 SPACE characters.
    Utf16StringBuilder builder;
    for (size_t index = 0; index < items().size(); ++index) {
        auto serialization = items()[index]->matrix()->to_string();
        // AD-HOC: The serialization of a matrix object throws if one of its components is not finite, and the spec
        //         does not say what to do then. We leave out such an item.
        if (serialization.is_error())
            continue;
        if (!builder.is_empty())
            builder.append_code_unit(' ');
        builder.append(serialization.value());
    }

    TemporaryChange change { m_is_reserializing, true };
    m_element->set_attribute_value(m_reflected_attribute, builder.to_string());
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransformList__createSVGTransformFromMatrix
WebIDL::ExceptionOr<GC::Ref<SVGTransform>> SVGTransformList::create_svg_transform_from_matrix(Bindings::DOMMatrix2DInit const& matrix)
{
    // 1. Let transform be a newly created SVGTransform object that is detached.
    auto transform = SVGTransform::create();

    // 2. Follow the steps that would be run if the setMatrix method on transform were called, passing matrix as its
    //    argument.
    TRY(transform->set_matrix(matrix));

    // 3. Return transform.
    return transform;
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransformList__consolidate
WebIDL::ExceptionOr<GC::Ptr<SVGTransform>> SVGTransformList::consolidate()
{
    // 1. If the SVGTransformList object is read only, then throw a NoModificationAllowedError.
    if (read_only() == ReadOnlyList::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify a read-only list"_utf16);

    // 2. If the list is empty, return null.
    if (items().is_empty())
        return nullptr;

    // 3. Detach and then remove all elements in the list.
    // NB: The matrix is computed from the elements before they are removed.
    // 4. Let transform be a newly created SVGTransform object.
    // 5. Let matrix be the matrix value obtained by beginning with an identity matrix, and then post-multiplying the
    //    value of the matrix object for each SVGTransform in the list, in order.
    auto matrix = Geometry::DOMMatrix::create();
    for (auto const& item : items())
        matrix->multiply_self(item->matrix());
    for (auto const& item : items())
        detach(item);
    mutable_items().clear();

    // 6. Set the components of transform's matrix object to the component values in matrix.
    // 7. If transform's matrix object would return true from its is2d method, then set transform's value to a
    //    matrix(…) value that represents the same matrix as the matrix object.
    // 8. Otherwise, set transform's value to a matrix3d(…) value that represents the same matrix as the matrix object.
    auto transform = SVGTransform::create(SVGTransform::Value::from_matrix(matrix->elements(), matrix->is2d()));

    // 9. Attach transform to this SVGTransformList.
    attach(transform);

    // 10. Append transform to this list.
    mutable_items().append(transform);

    // 11. If the list reflects a presentation attribute, then reserialize the reflected attribute.
    reserialize_reflected_attribute();

    // 12. Return transform.
    return transform;
}

}
