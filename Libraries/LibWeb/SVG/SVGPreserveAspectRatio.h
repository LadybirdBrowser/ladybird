/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/coords.html#InterfaceSVGPreserveAspectRatio
class SVGPreserveAspectRatio final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGPreserveAspectRatio, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGPreserveAspectRatio);

public:
    static constexpr u16 SVG_PRESERVEASPECTRATIO_UNKNOWN = 0;
    static constexpr u16 SVG_PRESERVEASPECTRATIO_NONE = 1;
    static constexpr u16 SVG_PRESERVEASPECTRATIO_XMAXYMAX = 10;

    static constexpr u16 SVG_MEETORSLICE_UNKNOWN = 0;
    static constexpr u16 SVG_MEETORSLICE_MEET = 1;
    static constexpr u16 SVG_MEETORSLICE_SLICE = 2;

    enum class ReadOnly : u8 {
        Yes,
        No,
    };

    [[nodiscard]] static GC::Ref<SVGPreserveAspectRatio> create(GC::Ref<SVGElement>, ReadOnly);
    virtual ~SVGPreserveAspectRatio() override;

    u16 align() const;
    WebIDL::ExceptionOr<void> set_align(u16);

    u16 meet_or_slice() const;
    WebIDL::ExceptionOr<void> set_meet_or_slice(u16);

private:
    SVGPreserveAspectRatio(GC::Ref<SVGElement>, ReadOnly);

    virtual void visit_edges(Cell::Visitor&) override;

    PreserveAspectRatio value() const;

    GC::Ref<SVGElement> m_element;
    ReadOnly m_read_only;
};

}
