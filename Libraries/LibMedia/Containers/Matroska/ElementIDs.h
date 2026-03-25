/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Media::Matroska {

// RFC 8794 - Extensible Binary Meta Language
// https://datatracker.ietf.org/doc/html/rfc8794
constexpr u32 EBML_MASTER_ELEMENT_ID = 0x1A45DFA3;
constexpr u32 EBML_CRC32_ELEMENT_ID = 0xBF;
constexpr u32 EBML_VOID_ELEMENT_ID = 0xEC;

// Matroska elements' IDs and types are listed at this URL:
// https://www.matroska.org/technical/elements.html

// Segment
constexpr u32 SEGMENT_ELEMENT_ID = 0x18538067;

// EBML Header
constexpr u32 DOCTYPE_ELEMENT_ID = 0x4282;
constexpr u32 DOCTYPE_VERSION_ELEMENT_ID = 0x4287;

// SeekHead
constexpr u32 SEEK_HEAD_ELEMENT_ID = 0x114D9B74;
constexpr u32 SEEK_ELEMENT_ID = 0x4DBB;
constexpr u32 SEEK_ID_ELEMENT_ID = 0x53AB;
constexpr u32 SEEK_POSITION_ELEMENT_ID = 0x53AC;

// Segment Information
constexpr u32 SEGMENT_INFORMATION_ELEMENT_ID = 0x1549A966;
constexpr u32 TIMESTAMP_SCALE_ID = 0x2AD7B1;
constexpr u32 MUXING_APP_ID = 0x4D80;
constexpr u32 WRITING_APP_ID = 0x5741;
constexpr u32 DURATION_ID = 0x4489;

// Tracks
constexpr u32 TRACK_ELEMENT_ID = 0x1654AE6B;
constexpr u32 TRACK_ENTRY_ID = 0xAE;
constexpr u32 TRACK_NUMBER_ID = 0xD7;
constexpr u32 TRACK_UID_ID = 0x73C5;
constexpr u32 TRACK_TYPE_ID = 0x83;
constexpr u32 TRACK_FLAG_DEFAULT_ID = 0x88;
constexpr u32 TRACK_NAME_ID = 0x536E;
constexpr u32 TRACK_LANGUAGE_ID = 0x22B59C;
constexpr u32 TRACK_LANGUAGE_BCP_47_ID = 0x22B59D;
constexpr u32 TRACK_CODEC_ID = 0x86;
constexpr u32 TRACK_CODEC_PRIVATE_ID = 0x63A2;
constexpr u32 TRACK_CODEC_DELAY_ID = 0x56AA;
constexpr u32 TRACK_SEEK_PRE_ROLL_ID = 0x56BB;
constexpr u32 TRACK_TIMESTAMP_SCALE_ID = 0x23314F;
constexpr u32 TRACK_OFFSET_ID = 0x537F;
constexpr u32 TRACK_DEFAULT_DURATION_ID = 0x23E383;
constexpr u32 TRACK_VIDEO_ID = 0xE0;
constexpr u32 TRACK_AUDIO_ID = 0xE1;

// Video
constexpr u32 PIXEL_WIDTH_ID = 0xB0;
constexpr u32 PIXEL_HEIGHT_ID = 0xBA;
constexpr u32 COLOR_ENTRY_ID = 0x55B0;
constexpr u32 PRIMARIES_ID = 0x55BB;
constexpr u32 TRANSFER_CHARACTERISTICS_ID = 0x55BA;
constexpr u32 MATRIX_COEFFICIENTS_ID = 0x55B1;
constexpr u32 RANGE_ID = 0x55B9;
constexpr u32 BITS_PER_CHANNEL_ID = 0x55B2;

// Audio
constexpr u32 CHANNELS_ID = 0x9F;
constexpr u32 SAMPLING_FREQUENCY_ID = 0xB5;
constexpr u32 BIT_DEPTH_ID = 0x6264;

// Clusters
constexpr u32 CLUSTER_ELEMENT_ID = 0x1F43B675;
constexpr u32 SIMPLE_BLOCK_ID = 0xA3;
constexpr u32 TIMESTAMP_ID = 0xE7;
constexpr u32 BLOCK_GROUP_ID = 0xA0;
constexpr u32 BLOCK_ID = 0xA1;
constexpr u32 BLOCK_DURATION_ID = 0x9B;

// Cues
constexpr u32 CUES_ID = 0x1C53BB6B;
constexpr u32 CUE_POINT_ID = 0xBB;
constexpr u32 CUE_TIME_ID = 0xB3;
constexpr u32 CUE_TRACK_POSITIONS_ID = 0xB7;
constexpr u32 CUE_TRACK_ID = 0xF7;
constexpr u32 CUE_CLUSTER_POSITION_ID = 0xF1;
constexpr u32 CUE_RELATIVE_POSITION_ID = 0xF0;
constexpr u32 CUE_CODEC_STATE_ID = 0xEA;
constexpr u32 CUE_REFERENCE_ID = 0xDB;

// Chapters
constexpr u32 CHAPTERS_ELEMENT_ID = 0x1043A770;

}
