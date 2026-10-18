/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

class SVGMPathElement final
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_WRAPPABLE(SVGMPathElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGMPathElement);

public:
    GC::Ptr<DOM::Element> referenced_element();

private:
    SVGMPathElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    static void motion_path_changed(GC::Ptr<Node> parent);
};

}
