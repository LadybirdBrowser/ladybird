/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// A keyframe of the animation of an SVG animation element, whose offset is always specified.
struct SVGKeyframe {
    double offset { 0 };
    Optional<CSS::EasingFunction> easing;
    HashMap<CSS::PropertyNameAndID, CSS::RustStyleValueHandle> properties;
};

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

    WebIDL::ExceptionOr<float> get_start_time();
    float get_current_time();
    WebIDL::ExceptionOr<float> get_simple_duration();

    void begin_element();
    void begin_element_at(float offset);
    void end_element();
    void end_element_at(float offset);

    void queue_time_events_if_necessary();
    void dispatch_pending_time_event(Animations::RustFFI::FfiPendingAnimationEvent const&);

protected:
    SVGAnimationElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Optional<CSS::PropertyID> target_property() const;
    virtual Vector<SVGKeyframe> keyframes(SVGElement const& target, CSS::PropertyID) const = 0;
    static SVGKeyframe keyframe(double offset, CSS::PropertyID, CSS::StyleValue const&);
    static void set_keyframe_property(SVGKeyframe&, CSS::PropertyID, CSS::StyleValue const&);

    double simple_duration() const;

    void update_animation();

private:
    GC::Ptr<SMILTimeContainer> time_container();
    double repeat_count() const;
    Animations::RustFFI::FillMode fill_mode() const;

    enum class Restart : u8 {
        Always,
        WhenNotActive,
        Never,
    };
    Restart restart() const;

    double current_presentation_time();
    Optional<double> begin_time_from_begin_attribute() const;
    Optional<double> interval_begin_time() const;
    Optional<double> interval_end_time() const;
    bool is_active_at(double time) const;
    void queue_time_event(Animations::RustFFI::TimeEventType, i32 detail = 0);

    // NB: The animation of the element, which the element holds until it lets go of it, and the time container of the
    //     timeline it plays on.
    Optional<Animations::AnimationRef> m_animation;
    GC::Ptr<SMILTimeContainer> m_animation_time_container;

    // NB: The latest instance times that the DOM methods created, in milliseconds on the timeline of the SVG document
    //     fragment.
    Optional<double> m_begin_instance_time_from_dom;
    Optional<double> m_end_instance_time_from_dom;

    // NB: What the time events that were last queued said about the interval, and when they were queued.
    bool m_time_events_reported_active { false };
    Optional<double> m_time_events_begin_time;
    double m_time_events_iteration { 0 };
    Optional<double> m_time_events_last_checked_time;
};

}
