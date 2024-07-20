/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGViewport.h>

namespace Web::SVG {

class SVGSymbolElement final : public SVGGraphicsElement
    , public SVGViewport {
    WEB_PLATFORM_OBJECT(SVGSymbolElement, SVGGraphicsElement);
    JS_DECLARE_ALLOCATOR(SVGSymbolElement);

public:
    virtual ~SVGSymbolElement() override = default;

    void apply_presentational_hints(CSS::StyleProperties& style) const override;

    virtual Optional<ViewBox> view_box() const override { return m_view_box; }
    virtual Optional<PreserveAspectRatio> preserve_aspect_ratio() const override
    {
        // FIXME: Support the `preserveAspectRatio` attribute on <symbol>.
        return {};
    }

    JS::NonnullGCPtr<SVGAnimatedRect> view_box_for_bindings() { return *m_view_box_for_bindings; }

private:
    SVGSymbolElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

    bool is_direct_child_of_use_shadow_tree() const;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    Optional<ViewBox> m_view_box;

    JS::GCPtr<SVGAnimatedRect> m_view_box_for_bindings;
};

}
