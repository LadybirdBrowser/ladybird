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

    WebIDL::ExceptionOr<float> get_start_time();
    float get_current_time();
    WebIDL::ExceptionOr<float> get_simple_duration();

    void begin_element();
    void begin_element_at(float offset);
    void end_element();
    void end_element_at(float offset);

    void queue_time_events_if_necessary();

protected:
    SVGAnimationElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Optional<CSS::PropertyID> target_property() const;
    virtual Vector<Animations::BaseKeyframe> keyframes(SVGElement const& target, CSS::PropertyID) const = 0;
    static Animations::BaseKeyframe keyframe(double offset, CSS::PropertyID, CSS::StyleValue const&);
    static void set_keyframe_property(Animations::BaseKeyframe&, CSS::PropertyID, CSS::StyleValue const&);

    double simple_duration() const;

    void update_animation();

private:
    GC::Ptr<SMILTimeContainer> time_container();
    double repeat_count() const;
    Bindings::FillMode fill_mode() const;

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
    void queue_time_event(Utf16FlyString const& type, i32 detail = 0);

    GC::Ptr<SMILAnimation> m_animation;

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
