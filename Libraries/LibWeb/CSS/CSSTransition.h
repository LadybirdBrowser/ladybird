/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/Time.h>

namespace Web::CSS {

class CSSTransition : public Animations::Animation {
    WEB_PLATFORM_OBJECT(CSSTransition, Animations::Animation);
    GC_DECLARE_ALLOCATOR(CSSTransition);

public:
    static GC::Ref<CSSTransition> start_a_transition(DOM::Element&, Optional<PseudoElement>, PropertyID,
        size_t transition_generation, double start_time, double end_time, NonnullRefPtr<CSSStyleValue const> start_value,
        NonnullRefPtr<CSSStyleValue const> end_value, NonnullRefPtr<CSSStyleValue const> reversing_adjusted_start_value, double reversing_shortening_factor);

    StringView transition_property() const { return string_from_property_id(m_transition_property); }

    virtual Animations::AnimationClass animation_class() const override;
    virtual Optional<int> class_specific_composite_order(GC::Ref<Animations::Animation> other) const override;

    double transition_start_time() const { return m_start_time; }
    double transition_end_time() const { return m_end_time; }
    NonnullRefPtr<CSSStyleValue const> transition_start_value() const { return m_start_value; }
    NonnullRefPtr<CSSStyleValue const> transition_end_value() const { return m_end_value; }
    NonnullRefPtr<CSSStyleValue const> reversing_adjusted_start_value() const { return m_reversing_adjusted_start_value; }
    double reversing_shortening_factor() const { return m_reversing_shortening_factor; }

    double timing_function_output_at_time(double t) const;

    // This is designed to be created from AnimationEffect::Phase.
    enum class Phase : u8 {
        Before,
        Active,
        After,
        Idle,
        Pending,
    };
    Phase previous_phase() const { return m_previous_phase; }
    void set_previous_phase(Phase phase) { m_previous_phase = phase; }

private:
    CSSTransition(JS::Realm&, DOM::Element&, Optional<PseudoElement>, PropertyID, size_t transition_generation,
        double start_time, double end_time, NonnullRefPtr<CSSStyleValue const> start_value, NonnullRefPtr<CSSStyleValue const> end_value,
        NonnullRefPtr<CSSStyleValue const> reversing_adjusted_start_value, double reversing_shortening_factor);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_css_transition() const override { return true; }

    PropertyID m_transition_property;

    // https://drafts.csswg.org/css-transitions-2/#transition-generation
    size_t m_transition_generation;

    // https://drafts.csswg.org/css-transitions/#transition-start-time
    double m_start_time;

    // https://drafts.csswg.org/css-transitions/#transition-end-time
    double m_end_time;

    // https://drafts.csswg.org/css-transitions/#transition-start-value
    NonnullRefPtr<CSS::CSSStyleValue const> m_start_value;

    // https://drafts.csswg.org/css-transitions/#transition-end-value
    NonnullRefPtr<CSS::CSSStyleValue const> m_end_value;

    // https://drafts.csswg.org/css-transitions/#transition-reversing-adjusted-start-value
    NonnullRefPtr<CSS::CSSStyleValue const> m_reversing_adjusted_start_value;

    // https://drafts.csswg.org/css-transitions/#transition-reversing-shortening-factor
    double m_reversing_shortening_factor;

    GC::Ref<Animations::KeyframeEffect> m_keyframe_effect;

    GC::Ptr<CSS::CSSStyleDeclaration const> m_cached_declaration;

    Phase m_previous_phase { Phase::Idle };
};

}
