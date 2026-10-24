/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/SVG/SVGList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGStringList
class SVGStringList final
    : public Bindings::GCAllocatedWrappable
    , public SVGList<Utf16String> {
    WEB_WRAPPABLE(SVGStringList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGStringList);

public:
    // How the reflected attribute separates its strings.
    enum class Separator : u8 {
        Comma,
        Whitespace,
    };

    [[nodiscard]] static GC::Ref<SVGStringList> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, Separator);
    virtual ~SVGStringList() override = default;

    static Vector<Utf16String> parse(Utf16View, Separator);

    // https://svgwg.org/svg2-draft/types.html#TermSynchronizeList
    void synchronize();

private:
    SVGStringList(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, Separator);

    virtual void visit_edges(Visitor&) override;

    virtual void reserialize_reflected_attribute() override;

    GC::Ref<SVGElement> m_element;
    Utf16FlyString m_reflected_attribute;
    Separator m_separator;
    bool m_is_reserializing { false };
};

}
