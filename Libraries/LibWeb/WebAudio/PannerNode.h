/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PannerNodePrototype.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#PannerOptions
struct PannerOptions : AudioNodeOptions {
    Bindings::PanningModelType panning_model { Bindings::PanningModelType::Equalpower };
    Bindings::DistanceModelType distance_model { Bindings::DistanceModelType::Inverse };
    float position_x { 0.0f };
    float position_y { 0.0f };
    float position_z { 0.0f };
    float orientation_x { 1.0f };
    float orientation_y { 0.0f };
    float orientation_z { 0.0f };
    double ref_distance { 1.0 };
    double max_distance { 10000.0 };
    double rolloff_factor { 1.0 };
    double cone_inner_angle { 360.0 };
    double cone_outer_angle { 360.0 };
    double cone_outer_gain { 0.0 };
};

// https://webaudio.github.io/web-audio-api/#PannerNode
class PannerNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(PannerNode, AudioNode);
    GC_DECLARE_ALLOCATOR(PannerNode);

public:
    virtual ~PannerNode() override;

    static WebIDL::ExceptionOr<GC::Ref<PannerNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, PannerOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<PannerNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, PannerOptions const& = {});

    WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<AudioParam const> position_x() const { return m_position_x; }
    GC::Ref<AudioParam const> position_y() const { return m_position_y; }
    GC::Ref<AudioParam const> position_z() const { return m_position_z; }
    GC::Ref<AudioParam const> orientation_x() const { return m_orientation_x; }
    GC::Ref<AudioParam const> orientation_y() const { return m_orientation_y; }
    GC::Ref<AudioParam const> orientation_z() const { return m_orientation_z; }

    Bindings::PanningModelType panning_model() const { return m_panning_model; }
    void set_panning_model(Bindings::PanningModelType value) { m_panning_model = value; }

    Bindings::DistanceModelType distance_model() const { return m_distance_model; }
    void set_distance_model(Bindings::DistanceModelType value) { m_distance_model = value; }

    double ref_distance() const { return m_ref_distance; }
    WebIDL::ExceptionOr<void> set_ref_distance(double);

    double max_distance() const { return m_max_distance; }
    WebIDL::ExceptionOr<void> set_max_distance(double);

    double rolloff_factor() const { return m_rolloff_factor; }
    WebIDL::ExceptionOr<void> set_rolloff_factor(double);

    double cone_inner_angle() const { return m_cone_inner_angle; }
    void set_cone_inner_angle(double value) { m_cone_inner_angle = value; }

    double cone_outer_angle() const { return m_cone_outer_angle; }
    void set_cone_outer_angle(double value) { m_cone_outer_angle = value; }

    double cone_outer_gain() const { return m_cone_outer_gain; }
    WebIDL::ExceptionOr<void> set_cone_outer_gain(double);

    WebIDL::ExceptionOr<void> set_position(float x, float y, float z);
    WebIDL::ExceptionOr<void> set_orientation(float x, float y, float z);

protected:
    PannerNode(JS::Realm&, GC::Ref<BaseAudioContext>, PannerOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://webaudio.github.io/web-audio-api/#dom-pannernode-panningmodel
    Bindings::PanningModelType m_panning_model { Bindings::PanningModelType::Equalpower };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-positionx
    GC::Ref<AudioParam> m_position_x;

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-positiony
    GC::Ref<AudioParam> m_position_y;

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-positionz
    GC::Ref<AudioParam> m_position_z;

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-orientationx
    GC::Ref<AudioParam> m_orientation_x;

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-orientationy
    GC::Ref<AudioParam> m_orientation_y;

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-orientationz
    GC::Ref<AudioParam> m_orientation_z;

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-distancemodel
    Bindings::DistanceModelType m_distance_model { Bindings::DistanceModelType::Inverse };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-refdistance
    double m_ref_distance { 1.0 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-maxdistance
    double m_max_distance { 10000.0 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-rollofffactor
    double m_rolloff_factor { 1.0 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneinnerangle
    double m_cone_inner_angle { 360.0 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneouterangle
    double m_cone_outer_angle { 360.0 };

    // https://webaudio.github.io/web-audio-api/#dom-pannernode-coneoutergain
    double m_cone_outer_gain { 0.0 };
};

}
