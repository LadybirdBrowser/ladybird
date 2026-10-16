/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/Codecs/H264.h>
#include <LibMediaCodecs/Codecs/H265.h>
#include <LibMediaCodecs/Codecs/NALUnit.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/NonReferenceFrameDetector.h>

namespace Media {

bool NonReferenceFrameDetector::frame_is_non_reference(CodedFrame const& frame)
{
    if (auto configuration = frame.new_codec_configuration(); configuration.has_value()) {
        m_codec_id = frame.codec_id();
        apply_codec_configuration(*configuration);
    } else if (frame.codec_id() != m_codec_id) {
        m_codec_id = frame.codec_id();
        m_nal_unit_length_size = {};
    }

    switch (m_codec_id) {
    case CodecID::H264:
        return h264_frame_is_non_reference(frame.data());
    case CodecID::H265:
        return h265_frame_is_non_reference(frame.data());
    default:
        return false;
    }
}

void NonReferenceFrameDetector::apply_codec_configuration(ReadonlyBytes configuration)
{
    m_nal_unit_length_size = {};
    m_highest_temporal_id = 0;

    switch (m_codec_id) {
    case CodecID::H264: {
        auto sets = Codecs::H264::parse_parameter_sets_from_configuration_record(configuration);
        if (sets.has_value())
            m_nal_unit_length_size = sets->nal_unit_length_size;
        break;
    }
    case CodecID::H265: {
        auto sets = Codecs::H265::parse_parameter_sets_from_configuration_record(configuration);
        if (!sets.has_value())
            break;
        for (auto nal_unit : sets->sequence) {
            auto sequence_set = Codecs::H265::parse_sequence_parameter_set(nal_unit);
            if (!sequence_set.has_value())
                return;
            m_highest_temporal_id = max(m_highest_temporal_id, sequence_set->sps_max_sub_layers_minus1);
        }
        m_nal_unit_length_size = sets->nal_unit_length_size;
        break;
    }
    default:
        break;
    }
}

bool NonReferenceFrameDetector::h264_frame_is_non_reference(ReadonlyBytes data) const
{
    if (!m_nal_unit_length_size.has_value())
        return false;

    // A parameter set must reach the decoder, whichever picture it travels with.
    auto saw_coded_slice = false;
    Codecs::NALUnitIterator iterator { data, m_nal_unit_length_size.value() };
    for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next()) {
        auto header = (*nal_unit)[0];
        if (Codecs::H264::is_parameter_set(header))
            return false;
        if (!Codecs::H264::is_coded_slice(header))
            continue;
        if (Codecs::H264::nal_ref_idc(header) != 0)
            return false;
        saw_coded_slice = true;
    }
    return saw_coded_slice && !iterator.has_error();
}

bool NonReferenceFrameDetector::h265_frame_is_non_reference(ReadonlyBytes data)
{
    if (!m_nal_unit_length_size.has_value())
        return false;

    // A parameter set must reach the decoder, whichever picture it travels with. Its SPS still has to be read, so
    // that the frames after it are judged by the sub-layers it allows.
    auto saw_parameter_set = false;
    auto saw_coded_slice = false;
    auto every_coded_slice_is_sub_layer_non_reference = true;
    u8 slice_temporal_id = 0;
    Codecs::NALUnitIterator iterator { data, m_nal_unit_length_size.value() };
    for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next()) {
        auto header = Codecs::H265::parse_nal_unit_header(*nal_unit);
        if (!header.has_value())
            continue;
        if (Codecs::H265::is_parameter_set(*header)) {
            saw_parameter_set = true;
            if (header->nuh_layer_id != 0)
                continue;
            if (auto sequence_set = Codecs::H265::parse_sequence_parameter_set(*nal_unit); sequence_set.has_value())
                m_highest_temporal_id = max(m_highest_temporal_id, sequence_set->sps_max_sub_layers_minus1);
            continue;
        }
        if (!Codecs::H265::is_coded_slice(*header))
            continue;
        saw_coded_slice = true;
        slice_temporal_id = max(slice_temporal_id, header->temporal_id);
        if (!Codecs::H265::is_sub_layer_non_reference(*header))
            every_coded_slice_is_sub_layer_non_reference = false;
    }
    if (iterator.has_error() || saw_parameter_set || !saw_coded_slice)
        return false;
    // ITU-T H.265 (07/2024), 7.4.2.2: an SLNR picture is only discardable for pictures at its own TemporalId.
    return every_coded_slice_is_sub_layer_non_reference && slice_temporal_id == m_highest_temporal_id;
}

}
