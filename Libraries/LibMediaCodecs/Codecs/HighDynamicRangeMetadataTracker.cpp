/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/Codecs/AV1.h>
#include <LibMediaCodecs/Codecs/H264.h>
#include <LibMediaCodecs/Codecs/H265.h>
#include <LibMediaCodecs/Codecs/HighDynamicRangeMetadataTracker.h>
#include <LibMediaCodecs/Codecs/NALUnit.h>
#include <LibMediaCodecs/CodedFrame.h>

namespace Media::Codecs {

HighDynamicRangeMetadataTracker::HighDynamicRangeMetadataTracker(CodecID codec_id, ReadonlyBytes configuration)
    : m_codec_id(codec_id)
{
    apply_configuration(configuration);
}

void HighDynamicRangeMetadataTracker::apply_configuration(ReadonlyBytes configuration)
{
    if (configuration.is_empty())
        return;
    forget_stream();

    switch (m_codec_id) {
    case CodecID::H264:
        if (auto sets = H264::parse_parameter_sets_from_configuration_record(configuration); sets.has_value())
            m_nal_unit_length_size = sets->nal_unit_length_size;
        break;
    case CodecID::H265:
        if (auto sets = H265::parse_parameter_sets_from_configuration_record(configuration); sets.has_value())
            m_nal_unit_length_size = sets->nal_unit_length_size;
        break;
    default:
        break;
    }
}

ColorMetadata const& HighDynamicRangeMetadataTracker::apply_coded_frame(CodedFrame const& coded_frame)
{
    if (auto configuration = coded_frame.new_codec_configuration(); configuration.has_value())
        apply_configuration(*configuration);
    begin_frame(coded_frame);

    switch (m_codec_id) {
    case CodecID::H264:
    case CodecID::H265: {
        NALUnitIterator iterator { coded_frame.data(), m_nal_unit_length_size };
        for (auto nal_unit = iterator.next(); nal_unit.has_value(); nal_unit = iterator.next())
            apply_nal_unit(*nal_unit);
        break;
    }
    case CodecID::AV1:
        take_bitstream_metadata(AV1::high_dynamic_range_metadata_of_obus(coded_frame.data()));
        break;
    default:
        break;
    }
    return m_metadata;
}

void HighDynamicRangeMetadataTracker::forget_stream()
{
    m_metadata = {};
    m_next_picture_begins_coded_video_sequence = false;
}

// A message replaces the one before it, so a level it leaves unknown does not fall back to the earlier message's.
void HighDynamicRangeMetadataTracker::take_bitstream_metadata(ColorMetadata const& metadata)
{
    if (metadata.mastering_display_color_volume.has_value())
        m_metadata.mastering_display_color_volume = metadata.mastering_display_color_volume;
    if (metadata.content_light_level_information.has_value())
        m_metadata.content_light_level_information = metadata.content_light_level_information;
    if (metadata.maximum_scene_luminance.has_value())
        m_metadata.maximum_scene_luminance = metadata.maximum_scene_luminance;
}

void HighDynamicRangeMetadataTracker::begin_frame(CodedFrame const& coded_frame)
{
    m_metadata.maximum_scene_luminance = coded_frame.maximum_scene_luminance();
    m_frame_gave_mastering_display_color_volume = false;
    m_frame_gave_content_light_level_information = false;
}

void HighDynamicRangeMetadataTracker::apply_nal_unit(ReadonlyBytes nal_unit)
{
    if (nal_unit.is_empty())
        return;

    ColorMetadata nal_unit_metadata;
    auto begins_coded_video_sequence = false;
    if (m_codec_id == CodecID::H264) {
        nal_unit_metadata = H264::high_dynamic_range_metadata_of_nal_unit(nal_unit);
        begins_coded_video_sequence = H264::begins_coded_video_sequence(nal_unit[0]);
    } else if (m_codec_id == CodecID::H265) {
        auto header = H265::parse_nal_unit_header(nal_unit);
        if (!header.has_value())
            return;
        if (H265::ends_coded_video_sequence(header.value())) {
            m_next_picture_begins_coded_video_sequence = true;
            return;
        }
        nal_unit_metadata = H265::high_dynamic_range_metadata_of_nal_unit(nal_unit);
        begins_coded_video_sequence = H265::begins_coded_video_sequence(header.value());
        if (H265::is_coded_slice(header.value()) && header->nuh_layer_id == 0)
            begins_coded_video_sequence |= exchange(m_next_picture_begins_coded_video_sequence, false);
    } else {
        return;
    }

    m_frame_gave_mastering_display_color_volume |= nal_unit_metadata.mastering_display_color_volume.has_value();
    m_frame_gave_content_light_level_information |= nal_unit_metadata.content_light_level_information.has_value();
    take_bitstream_metadata(nal_unit_metadata);

    // ITU-T H.265 (07/2024), D.3.28 and D.3.35: the messages only describe the coded video sequence they are part of.
    // They are prefix SEI messages, so any that the sequence's first picture carries came before this slice.
    if (begins_coded_video_sequence) {
        if (!m_frame_gave_mastering_display_color_volume)
            m_metadata.mastering_display_color_volume = {};
        if (!m_frame_gave_content_light_level_information)
            m_metadata.content_light_level_information = {};
    }
}

}
