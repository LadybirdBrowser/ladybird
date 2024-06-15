/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class CSSTransition : public Animations::Animation {
    WEB_PLATFORM_OBJECT(CSSTransition, Animations::Animation);
    JS_DECLARE_ALLOCATOR(CSSTransition);

public:
    static JS::NonnullGCPtr<CSSTransition> create(JS::Realm&, PropertyID, size_t transition_generation);

    StringView transition_property() const { return string_from_property_id(m_transition_property); }

    JS::GCPtr<DOM::Element> owning_element() const override { return m_owning_element; }
    void set_owning_element(JS::GCPtr<DOM::Element> value) { m_owning_element = value; }

    JS::GCPtr<CSS::CSSStyleDeclaration const> cached_declaration() const { return m_cached_declaration; }
    void set_cached_declaration(JS::GCPtr<CSS::CSSStyleDeclaration const> declaration) { m_cached_declaration = declaration; }

    virtual Animations::AnimationClass animation_class() const override;
    virtual Optional<int> class_specific_composite_order(JS::NonnullGCPtr<Animations::Animation> other) const override;

private:
    CSSTransition(JS::Realm&, PropertyID, size_t transition_generation);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_css_transition() const override { return true; }

    PropertyID m_transition_property;

    // https://drafts.csswg.org/css-transitions-2/#transition-generation
    size_t m_transition_generation;

    // https://drafts.csswg.org/css-transitions-2/#owning-element
    JS::GCPtr<DOM::Element> m_owning_element;

    JS::GCPtr<CSS::CSSStyleDeclaration const> m_cached_declaration;
};

}
