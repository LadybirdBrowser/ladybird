/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#InterfaceSVGNumber
class SVGNumber final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGNumber, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGNumber);

public:
    enum class ReadOnly : u8 {
        Yes,
        No,
    };

    [[nodiscard]] static GC::Ref<SVGNumber> create(float value, ReadOnly);
    virtual ~SVGNumber() override = default;

    float value() const { return m_value; }
    WebIDL::ExceptionOr<void> set_value(float value);

    ReadOnly read_only() const { return m_read_only; }

    // https://svgwg.org/svg2-draft/types.html#TermAttach
    void attach(GC::Ref<SVGNumberList>, ReadOnly);

    // https://svgwg.org/svg2-draft/types.html#TermDetach
    void detach();

    bool is_detached() const { return !m_list; }

    // Sets the value when the list this number belongs to is synchronized with its reflected attribute.
    void set_value_from_reflected_attribute(float value) { m_value = value; }

private:
    SVGNumber(float value, ReadOnly);

    virtual void visit_edges(Cell::Visitor&) override;

    float m_value { 0 };

    // The list this number is an element of, if it reflects an element of the base value of a reflected attribute.
    GC::Ptr<SVGNumberList> m_list;

    // https://www.w3.org/TR/SVG2/types.html#ReadOnlyNumber
    ReadOnly m_read_only;
};

}
