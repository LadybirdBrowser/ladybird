/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/OscillatorGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/OscillatorRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> OscillatorGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_u8(static_cast<u8>(type)));
    TRY(encoder.append_f32(frequency));
    TRY(encoder.append_f32(detune_cents));
    TRY(append_optional_size_as_u64(encoder, start_frame));
    TRY(append_optional_size_as_u64(encoder, stop_frame));

    TRY(encoder.append_u8(periodic_wave.has_value()));
    if (periodic_wave.has_value()) {
        auto const& coeffs = periodic_wave.value();
        TRY(encoder.append_u8(coeffs.normalize ? 1 : 0));

        if (coeffs.real.size() > NumericLimits<u32>::max() || coeffs.imag.size() > NumericLimits<u32>::max())
            return Error::from_string_literal("PeriodicWave coefficient vector too large for wire encoding");

        TRY(encoder.append_u32(static_cast<u32>(coeffs.real.size())));
        for (auto value : coeffs.real)
            TRY(encoder.append_f32(value));

        TRY(encoder.append_u32(static_cast<u32>(coeffs.imag.size())));
        for (auto value : coeffs.imag)
            TRY(encoder.append_f32(value));
    }
    return {};
}

ErrorOr<OscillatorGraphNode> OscillatorGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    OscillatorGraphNode node;
    node.type = static_cast<OscillatorType>(TRY(decoder.read_u8()));
    node.frequency = TRY(decoder.read_f32());
    node.detune_cents = TRY(decoder.read_f32());
    node.start_frame = TRY(read_optional_size_from_u64(decoder));
    node.stop_frame = TRY(read_optional_size_from_u64(decoder));

    auto has_periodic_wave = TRY(decoder.read_u8());
    if (has_periodic_wave) {
        PeriodicWaveCoefficients coeffs;
        coeffs.normalize = TRY(decoder.read_u8()) != 0;

        auto real_size = TRY(decoder.read_u32());
        auto imag_size = TRY(decoder.read_u32());

        TRY(coeffs.real.try_resize(real_size));
        TRY(coeffs.imag.try_resize(imag_size));

        for (u32 i = 0; i < real_size; ++i)
            coeffs.real[i] = TRY(decoder.read_f32());
        for (u32 i = 0; i < imag_size; ++i)
            coeffs.imag[i] = TRY(decoder.read_f32());

        node.periodic_wave = move(coeffs);
    }
    return node;
}

OwnPtr<RenderNode> OscillatorGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<OscillatorRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind OscillatorGraphNode::classify_update(OscillatorGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (type != new_desc.type)
        return GraphUpdateKind::RebuildRequired;

    if (periodic_wave.has_value() != new_desc.periodic_wave.has_value())
        return GraphUpdateKind::RebuildRequired;
    if (periodic_wave.has_value()) {
        auto const& lhs = periodic_wave.value();
        auto const& rhs = new_desc.periodic_wave.value();
        if (lhs.normalize != rhs.normalize)
            return GraphUpdateKind::RebuildRequired;
        if (lhs.real.size() != rhs.real.size() || lhs.imag.size() != rhs.imag.size())
            return GraphUpdateKind::RebuildRequired;
        for (size_t i = 0; i < lhs.real.size(); ++i) {
            if (lhs.real[i] != rhs.real[i])
                return GraphUpdateKind::RebuildRequired;
        }
        for (size_t i = 0; i < lhs.imag.size(); ++i) {
            if (lhs.imag[i] != rhs.imag[i])
                return GraphUpdateKind::RebuildRequired;
        }
    }

    if (frequency != new_desc.frequency)
        return GraphUpdateKind::Parameter;
    if (detune_cents != new_desc.detune_cents)
        return GraphUpdateKind::Parameter;
    if (start_frame != new_desc.start_frame)
        return GraphUpdateKind::Parameter;
    if (stop_frame != new_desc.stop_frame)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
