/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Export.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGAnimationElement : public SVGElement {
    WEB_WRAPPABLE(SVGAnimationElement, SVGElement);

public:
    GC::Ptr<SVGElement> target_element();

    void set_onbegin(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onbegin();

    void set_onend(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onend();

    void set_onrepeat(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onrepeat();

protected:
    SVGAnimationElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Vector<Animations::BaseKeyframe> keyframes(SVGElement const& target, CSS::PropertyID) const = 0;

    void update_animation();

private:
    Optional<CSS::PropertyID> target_property() const;

    GC::Ptr<SMILAnimation> m_animation;
};

}
