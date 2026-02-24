/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/AudioListenerRenderNode.h>
#include <LibWeb/WebAudio/RenderNodes/PannerRenderNode.h>

namespace Web::WebAudio::Render {

PannerRenderNode::PannerRenderNode(NodeID node_id, size_t quantum_size, PannerGraphNode const& desc)
    : RenderNode(node_id)
    , m_panning_model(desc.panning_model)
    , m_distance_model(desc.distance_model)
    , m_ref_distance(desc.ref_distance)
    , m_max_distance(desc.max_distance)
    , m_rolloff_factor(desc.rolloff_factor)
    , m_cone_inner_angle(desc.cone_inner_angle)
    , m_cone_outer_angle(desc.cone_outer_angle)
    , m_cone_outer_gain(desc.cone_outer_gain)
    , m_position_x(desc.position_x)
    , m_position_y(desc.position_y)
    , m_position_z(desc.position_z)
    , m_orientation_x(desc.orientation_x)
    , m_orientation_y(desc.orientation_y)
    , m_orientation_z(desc.orientation_z)
    , m_channel_count(desc.channel_count)
    , m_channel_count_mode(desc.channel_count_mode)
    , m_channel_interpretation(desc.channel_interpretation)
    , m_output_bus(2, quantum_size)
{
}

void PannerRenderNode::apply_description(GraphNodeDescription const& desc)
{
    ASSERT_RENDER_THREAD();

    if (!desc.has<PannerGraphNode>())
        return;
    PannerGraphNode const& panner = desc.get<PannerGraphNode>();
    m_panning_model = panner.panning_model;
    m_distance_model = panner.distance_model;
    m_ref_distance = panner.ref_distance;
    m_max_distance = panner.max_distance;
    m_rolloff_factor = panner.rolloff_factor;
    m_cone_inner_angle = panner.cone_inner_angle;
    m_cone_outer_angle = panner.cone_outer_angle;
    m_cone_outer_gain = panner.cone_outer_gain;
    m_position_x = panner.position_x;
    m_position_y = panner.position_y;
    m_position_z = panner.position_z;
    m_orientation_x = panner.orientation_x;
    m_orientation_y = panner.orientation_y;
    m_orientation_z = panner.orientation_z;
    m_channel_count = panner.channel_count;
    m_channel_count_mode = panner.channel_count_mode;
    m_channel_interpretation = panner.channel_interpretation;
}

static void apply_distance_model(DistanceModelType model, f32& distance, f64 ref_distance, f64 max_distance, f64 rolloff_factor)
{
    // https://webaudio.github.io/web-audio-api/#distance-effects

    switch (model) {
    case DistanceModelType::Linear:
        rolloff_factor = AK::clamp(rolloff_factor, 0.0, 1.0);
        distance = AK::clamp(distance, static_cast<f32>(ref_distance), static_cast<f32>(max_distance));
        distance = static_cast<f32>(1.0 - (rolloff_factor * (distance - ref_distance) / (max_distance - ref_distance)));
        break;
    case DistanceModelType::Inverse:
        if (distance < ref_distance)
            distance = 1.0f;
        else
            distance = static_cast<f32>(ref_distance / (ref_distance + rolloff_factor * (distance - ref_distance)));
        break;
    case DistanceModelType::Exponential:
        distance = AK::max(distance, static_cast<f32>(ref_distance));
        distance = static_cast<f32>(AK::pow(distance / ref_distance, -rolloff_factor));
        break;
    }
}

static void apply_cone_gain(f32& gain, f32 source_to_listener_x, f32 source_to_listener_y, f32 source_to_listener_z,
    f32 orientation_x, f32 orientation_y, f32 orientation_z, f64 inner_angle, f64 outer_angle, f64 outer_gain)
{
    // https://webaudio.github.io/web-audio-api/#sound-cones

    if (inner_angle == 360.0 && outer_angle == 360.0)
        return;

    // The angle between the source orientation vector and the source-listener vector
    // is calculated.
    // Normalized source orientation vector.
    f32 const len = AK::sqrt((orientation_x * orientation_x) + (orientation_y * orientation_y) + (orientation_z * orientation_z));
    if (len == 0.0f) {
        // "If the orientation vector is zero, the cone effect is not applied."
        return;
    }
    f32 const ox = orientation_x / len;
    f32 const oy = orientation_y / len;
    f32 const oz = orientation_z / len;

    // Normalized source-listener vector.
    f32 const sl_len = AK::sqrt((source_to_listener_x * source_to_listener_x) + (source_to_listener_y * source_to_listener_y) + (source_to_listener_z * source_to_listener_z));
    if (sl_len == 0.0f)
        return;
    f32 const slx = source_to_listener_x / sl_len;
    f32 const sly = source_to_listener_y / sl_len;
    f32 const slz = source_to_listener_z / sl_len;

    // Dot product gives cosine of angle.
    f32 const dot = (ox * slx) + (oy * sly) + (oz * slz);
    // Angle in degrees.
    f32 const angle = acos(dot) * 180.0f / AK::Pi<f32>;
    f32 const abs_angle = abs(angle);

    // If the angle is less than or equal to innerAngle/2, the gain is 1.
    if (abs_angle <= inner_angle / 2.0)
        return;

    // If the angle is greater than or equal to outerAngle/2, the gain is outerGain.
    if (abs_angle >= outer_angle / 2.0) {
        gain *= static_cast<f32>(outer_gain);
        return;
    }
    // Between the inner and outer angles, the gain is interpolated linearly.
    f64 const t = (abs_angle - inner_angle / 2.0) / (outer_angle / 2.0 - inner_angle / 2.0);
    gain *= static_cast<f32>((1.0 - t) + (outer_gain * t));
}

void PannerRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    // If no input, silence.
    if (!mixed_input) {
        m_output_bus.channel(0).fill(0.0f);
        m_output_bus.channel(1).fill(0.0f);
        return;
    }

    // Retrieve AudioParams.
    // positionX, positionY, positionZ, orientationX, orientationY, orientationZ.
    // If not connected, use intrinsic values (stored in param_inputs[index][0] if initialized).
    // GraphExecutor handles computedValue logic for us and puts it into param_inputs[index][0].

    // We need spans for params.
    auto get_param_span = [&](size_t index) -> Span<f32 const> {
        if (param_inputs.size() > index && !param_inputs[index].is_empty())
            return param_inputs[index][0]->channel(0);
        return {};
    };

    Span<f32 const> px = get_param_span(PannerParamIndex::position_x);
    Span<f32 const> py = get_param_span(PannerParamIndex::position_y);
    Span<f32 const> pz = get_param_span(PannerParamIndex::position_z);
    Span<f32 const> ox = get_param_span(PannerParamIndex::orientation_x);
    Span<f32 const> oy = get_param_span(PannerParamIndex::orientation_y);
    Span<f32 const> oz = get_param_span(PannerParamIndex::orientation_z);

    // Listener params.
    AudioListenerRenderNode* listener = context.listener;
    // Listener is always present in context.

    Span<f32 const> lx = listener->position_x();
    Span<f32 const> ly = listener->position_y();
    Span<f32 const> lz = listener->position_z();
    Span<f32 const> lfx = listener->forward_x();
    Span<f32 const> lfy = listener->forward_y();
    Span<f32 const> lfz = listener->forward_z();
    Span<f32 const> lux = listener->up_x();
    Span<f32 const> luy = listener->up_y();
    Span<f32 const> luz = listener->up_z();

    size_t const frames = mixed_input->frame_count();

    f32 const* in_l = mixed_input->channel(0).data();
    f32 const* in_r = mixed_input->channel_count() > 1 ? mixed_input->channel(1).data() : in_l; // Mono handled as duplicated

    Span<f32> out_l = m_output_bus.channel(0);
    Span<f32> out_r = m_output_bus.channel(1);

    bool const is_mono = mixed_input->channel_count() == 1;

    auto get_param_value = [](Span<f32 const> span, f32 intrinsic, size_t i) -> f32 {
        if (span.is_empty())
            return intrinsic;
        return span[i];
    };

    for (size_t i = 0; i < frames; ++i) {
        f32 const pos_x = get_param_value(px, m_position_x, i);
        f32 const pos_y = get_param_value(py, m_position_y, i);
        f32 const pos_z = get_param_value(pz, m_position_z, i);
        f32 const ori_x = get_param_value(ox, m_orientation_x, i);
        f32 const ori_y = get_param_value(oy, m_orientation_y, i);
        f32 const ori_z = get_param_value(oz, m_orientation_z, i);

        // 1. Calculate source-listener vector.
        f32 const sl_x = pos_x - lx[i];
        f32 const sl_y = pos_y - ly[i];
        f32 const sl_z = pos_z - lz[i];

        // 2. Apply distance and cone gain.
        f32 distance = AK::sqrt((sl_x * sl_x) + (sl_y * sl_y) + (sl_z * sl_z));
        f32 gain = 1.0f;

        apply_distance_model(m_distance_model, distance, m_ref_distance, m_max_distance, m_rolloff_factor);
        gain *= distance;

        apply_cone_gain(gain, sl_x, sl_y, sl_z, ori_x, ori_y, ori_z, m_cone_inner_angle, m_cone_outer_angle, m_cone_outer_gain);

        // 3. Azimuth and Elevation.
        // Transform the source position into the listener's coordinate system.
        // Listener basis is defined by forward and up vectors. We build an orthonormal frame:
        // forward_norm, right = normalize(cross(forward_norm, up_norm)), up_ortho = cross(right, forward_norm).
        auto normalize_vec = [](f32& x, f32& y, f32& z) {
            f32 const len = AK::sqrt((x * x) + (y * y) + (z * z));
            if (len > 0.0f) {
                x /= len;
                y /= len;
                z /= len;
            }
        };

        f32 lfx_v = lfx[i];
        f32 lfy_v = lfy[i];
        f32 lfz_v = lfz[i];
        if (lfx_v == 0.0f && lfy_v == 0.0f && lfz_v == 0.0f)
            lfz_v = -1.0f;
        normalize_vec(lfx_v, lfy_v, lfz_v);

        f32 lux_v = lux[i];
        f32 luy_v = luy[i];
        f32 luz_v = luz[i];
        if (lux_v == 0.0f && luy_v == 0.0f && luz_v == 0.0f)
            luy_v = 1.0f;
        normalize_vec(lux_v, luy_v, luz_v);

        // Right vector.
        f32 lrx_v = (lfy_v * luz_v) - (lfz_v * luy_v);
        f32 lry_v = (lfz_v * lux_v) - (lfx_v * luz_v);
        f32 lrz_v = (lfx_v * luy_v) - (lfy_v * lux_v);
        normalize_vec(lrx_v, lry_v, lrz_v);

        // Recompute up so the basis is orthogonal.
        f32 lupx_v = (lry_v * lfz_v) - (lrz_v * lfy_v);
        f32 lupy_v = (lrz_v * lfx_v) - (lrx_v * lfz_v);
        f32 lupz_v = (lrx_v * lfy_v) - (lry_v * lfx_v);
        normalize_vec(lupx_v, lupy_v, lupz_v);

        // Project source-listener vector onto listener basis vectors.
        // x = dot(sl, right)
        // y = dot(sl, up)
        // z = dot(sl, forward)
        f32 const p_l_x = (sl_x * lrx_v) + (sl_y * lry_v) + (sl_z * lrz_v);
        f32 const p_l_y = (sl_x * lupx_v) + (sl_y * lupy_v) + (sl_z * lupz_v);
        f32 const p_l_z = (sl_x * lfx_v) + (sl_y * lfy_v) + (sl_z * lfz_v);

        // Convert to azimuth and elevation (radians).
        f32 azimuth = (p_l_z == 0.0f && p_l_x == 0.0f) ? 0.0f : AK::atan2(p_l_x, p_l_z);
        f32 elevation = AK::atan2(p_l_y, AK::sqrt((p_l_x * p_l_x) + (p_l_z * p_l_z)));

        // Fold azimuth/elevation into the front hemisphere per spec.
        if (azimuth > AK::Pi<f32> / 2.0f)
            azimuth = AK::Pi<f32> - azimuth;
        else if (azimuth < -AK::Pi<f32> / 2.0f)
            azimuth = -AK::Pi<f32> - azimuth;

        if (elevation > AK::Pi<f32> / 2.0f)
            elevation = AK::Pi<f32> - elevation;
        else if (elevation < -AK::Pi<f32> / 2.0f)
            elevation = -AK::Pi<f32> - elevation;

        // 4. Equal-power panning. Folded azimuth is in radians; convert to degrees to match the spec formulas.
        f32 const azimuth_degrees = azimuth * 180.0f / AK::Pi<f32>;
        bool const is_center = AK::abs(azimuth_degrees) < 0.0001f;

        if (is_mono) {
            // Mono input: standard equal-power pan of the single channel.
            // pan position = (azimuth + 90 deg) / 180 deg
            f32 const pan_pos = clamp((azimuth_degrees + 90.0f) / 180.0f, 0.0f, 1.0f);
            f32 gain_l;
            f32 gain_r;

            if (is_center) {
                gain_l = 0.70710677f;
                gain_r = 0.70710677f;
            } else {
                // The double trig avoids collapsing to exact zeros; Golden ears won't notice but WPTs will.
                gain_l = static_cast<f32>(AK::cos(static_cast<f64>(pan_pos) * AK::Pi<f64> / 2.0));
                gain_r = static_cast<f32>(AK::sin(static_cast<f64>(pan_pos) * AK::Pi<f64> / 2.0));
            }

            f32 m = in_l[i] * gain;
            out_l[i] = m * gain_l;
            out_r[i] = m * gain_r;
        } else {
            // Stereo input: follow the spec mix rules instead of downmixing to mono.
            if (is_center) {
                // Avoid tiny near-center azimuth noise; just pass through with distance gain.
                out_l[i] = in_l[i] * gain;
                out_r[i] = in_r[i] * gain;
                continue;
            }

            f32 gain_l = 0.0f;
            f32 gain_r = 0.0f;
            f32 pan_pos = 0.0f;

            bool const source_on_left = azimuth_degrees <= 0.0f;
            if (source_on_left) {
                pan_pos = (azimuth_degrees + 90.0f) / 90.0f;
                pan_pos = clamp(pan_pos, 0.0f, 1.0f);
                gain_l = static_cast<f32>(AK::cos(static_cast<f64>(pan_pos) * AK::Pi<f64> / 2.0));
                gain_r = static_cast<f32>(AK::sin(static_cast<f64>(pan_pos) * AK::Pi<f64> / 2.0));
            } else {
                pan_pos = azimuth_degrees / 90.0f;
                pan_pos = clamp(pan_pos, 0.0f, 1.0f);
                gain_l = static_cast<f32>(AK::cos(static_cast<f64>(pan_pos) * AK::Pi<f64> / 2.0));
                gain_r = static_cast<f32>(AK::sin(static_cast<f64>(pan_pos) * AK::Pi<f64> / 2.0));
            }

            f32 out_left = 0.0f;
            f32 out_right = 0.0f;

            if (source_on_left) {
                // Left channel keeps its original signal; right channel leaks into left.
                out_left = in_l[i] + in_r[i] * gain_l;
                out_right = in_r[i] * gain_r;
            } else {
                // Right side: left channel leaks into right.
                out_left = in_l[i] * gain_l;
                out_right = in_r[i] + in_l[i] * gain_r;
            }

            out_l[i] = out_left * gain;
            out_r[i] = out_right * gain;
        }
    }
}

AudioBus const& PannerRenderNode::output(size_t index) const
{
    ASSERT_RENDER_THREAD();
    // FIXME: index? Panner is always 1 output?
    (void)index;
    return m_output_bus;
}

}
