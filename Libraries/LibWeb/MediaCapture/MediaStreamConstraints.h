/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::MediaCapture {

// https://w3c.github.io/mediacapture-main/#dom-constraindomstringparameters
struct ConstrainDOMStringParameters {
    // The exact required value for this property.
    Optional<Variant<String, Vector<String>>> exact;
    // The ideal (target) value for this property.
    Optional<Variant<String, Vector<String>>> ideal;
};

// https://w3c.github.io/mediacapture-main/#dom-constraindoublerange
struct DoubleRange {
    // The maximum valid value of this property.
    Optional<double> max;
    // The minimum value of this property.
    Optional<double> min;
};

// https://w3c.github.io/mediacapture-main/#dom-constraindoublerange
struct ConstrainDoubleRange : DoubleRange {
    // The exact required value for this property.
    Optional<double> exact;
    // The ideal (target) value for this property.
    Optional<double> ideal;
};

// https://w3c.github.io/mediacapture-main/#dom-constrainulongrange
struct ULongRange {
    // The maximum valid value of this property.
    Optional<WebIDL::UnsignedLong> max;
    // The minimum value of this property.
    Optional<WebIDL::UnsignedLong> min;
};

// https://w3c.github.io/mediacapture-main/#dom-constrainulongrange
struct ConstrainULongRange : ULongRange {
    // The exact required value for this property.
    Optional<WebIDL::UnsignedLong> exact;
    // The ideal (target) value for this property.
    Optional<WebIDL::UnsignedLong> ideal;
};

// https://w3c.github.io/mediacapture-main/#dom-constrainbooleanparameters
struct ConstrainBooleanParameters {
    // The exact required value for this property.
    Optional<bool> exact;
    // The ideal (target) value for this property.
    Optional<bool> ideal;
};

// https://w3c.github.io/mediacapture-main/#dom-constrainbooleanordomstringparameters
struct ConstrainBooleanOrDOMStringParameters {
    // The exact required value for this property.
    Optional<Variant<bool, String>> exact;
    // The ideal (target) value for this property.
    Optional<Variant<bool, String>> ideal;
};

// https://w3c.github.io/mediacapture-main/#dom-constrainulong
// Throughout this specification, the identifier ConstrainULong is used to refer to the
// (unsigned long or ConstrainULongRange) type.
using ConstrainULong = Variant<WebIDL::UnsignedLong, ConstrainULongRange>;

// https://w3c.github.io/mediacapture-main/#dom-constraindouble
// Throughout this specification, the identifier ConstrainDouble is used to refer to the
// (double or ConstrainDoubleRange) type.
using ConstrainDouble = Variant<double, ConstrainDoubleRange>;

// https://w3c.github.io/mediacapture-main/#dom-constrainboolean
// Throughout this specification, the identifier ConstrainBoolean is used to refer to the
// (boolean or ConstrainBooleanParameters) type.
using ConstrainBoolean = Variant<bool, ConstrainBooleanParameters>;

// https://w3c.github.io/mediacapture-main/#dom-constraindomstring
// Throughout this specification, the identifier ConstrainDOMString is used to refer to the
// (DOMString or sequence<DOMString> or ConstrainDOMStringParameters) type.
using ConstrainDOMString = Variant<String, Vector<String>, ConstrainDOMStringParameters>;

// https://w3c.github.io/mediacapture-main/#dom-constrainbooleanordomstring
// Throughout this specification, the identifier ConstrainBooleanOrDOMString is used to refer to the
// (boolean or DOMString or ConstrainBooleanOrDOMStringParameters) type.
using ConstrainBooleanOrDOMString = Variant<bool, String, ConstrainBooleanOrDOMStringParameters>;

// https://w3c.github.io/mediacapture-main/#dictdef-mediatrackconstraintset
struct MediaTrackConstraintSet {
    Optional<ConstrainULong> width;
    Optional<ConstrainULong> height;
    Optional<ConstrainDouble> aspect_ratio;
    Optional<ConstrainDouble> frame_rate;
    Optional<ConstrainDOMString> facing_mode;
    Optional<ConstrainDOMString> resize_mode;
    Optional<ConstrainULong> sample_rate;
    Optional<ConstrainULong> sample_size;
    Optional<ConstrainBooleanOrDOMString> echo_cancellation;
    Optional<ConstrainBoolean> auto_gain_control;
    Optional<ConstrainBoolean> noise_suppression;
    Optional<ConstrainDouble> latency;
    Optional<ConstrainULong> channel_count;
    Optional<ConstrainDOMString> device_id;
    Optional<ConstrainDOMString> group_id;
    Optional<ConstrainBoolean> background_blur;
};

// https://w3c.github.io/mediacapture-main/#mediatrackconstraints
struct MediaTrackConstraints : MediaTrackConstraintSet {
    // This is the list of ConstraintSets that the User Agent MUST attempt to satisfy, in order,
    // skipping only those that cannot be satisfied.
    Optional<Vector<MediaTrackConstraintSet>> advanced;
};

// https://w3c.github.io/mediacapture-main/#mediastreamconstraints
struct MediaStreamConstraints {
    // If true, it requests that the returned MediaStream contain a video track. If a Constraints
    // structure is provided, it further specifies the nature and settings of the video Track.
    // If false, the MediaStream MUST NOT contain a video Track.
    Variant<bool, MediaTrackConstraints> video { false };

    // If true, it requests that the returned MediaStream contain an audio track. If a Constraints
    // structure is provided, it further specifies the nature and settings of the audio Track.
    // If false, the MediaStream MUST NOT contain an audio Track.
    Variant<bool, MediaTrackConstraints> audio { false };
};

// https://w3c.github.io/mediacapture-main/#dom-mediatracksupportedconstraints
struct MediaTrackSupportedConstraints {
    bool width { false };
    bool height { false };
    bool aspect_ratio { false };
    bool frame_rate { false };
    bool facing_mode { false };
    bool resize_mode { false };
    bool sample_rate { false };
    bool sample_size { false };
    bool echo_cancellation { false };
    bool auto_gain_control { false };
    bool noise_suppression { false };
    bool latency { false };
    bool channel_count { false };
    bool device_id { false };
    bool group_id { false };
    bool background_blur { false };
};

// https://w3c.github.io/mediacapture-main/#dom-mediatrackcapabilities
struct MediaTrackCapabilities {
    Optional<ULongRange> width;
    Optional<ULongRange> height;
    Optional<DoubleRange> aspect_ratio;
    Optional<DoubleRange> frame_rate;
    Optional<Vector<String>> facing_mode;
    Optional<Vector<String>> resize_mode;
    Optional<ULongRange> sample_rate;
    Optional<ULongRange> sample_size;
    Optional<Vector<Variant<bool, String>>> echo_cancellation;
    Optional<Vector<bool>> auto_gain_control;
    Optional<Vector<bool>> noise_suppression;
    Optional<DoubleRange> latency;
    Optional<ULongRange> channel_count;
    Optional<String> device_id;
    Optional<String> group_id;
    Optional<Vector<bool>> background_blur;
};

// https://w3c.github.io/mediacapture-main/#dom-mediatracksettings
struct MediaTrackSettings {
    Optional<WebIDL::UnsignedLong> width;
    Optional<WebIDL::UnsignedLong> height;
    Optional<double> aspect_ratio;
    Optional<double> frame_rate;
    Optional<String> facing_mode;
    Optional<String> resize_mode;
    Optional<WebIDL::UnsignedLong> sample_rate;
    Optional<WebIDL::UnsignedLong> sample_size;
    Optional<Variant<bool, String>> echo_cancellation;
    Optional<bool> auto_gain_control;
    Optional<bool> noise_suppression;
    Optional<double> latency;
    Optional<WebIDL::UnsignedLong> channel_count;
    Optional<String> device_id;
    Optional<String> group_id;
    Optional<bool> background_blur;
};

}
