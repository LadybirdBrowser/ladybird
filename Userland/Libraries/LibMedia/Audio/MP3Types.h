/*
 * Copyright (c) 2021, Arne Elster <arne@elster.li>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/EnumBits.h>
#include <AK/FixedArray.h>

namespace Audio::MP3 {

constexpr size_t const frame_size = 1152;
// 576 samples.
constexpr size_t const granule_size = frame_size / 2;

enum class Mode {
    Stereo = 0,
    JointStereo = 1,
    DualChannel = 2,
    SingleChannel = 3,
};

enum class ModeExtension {
    Stereo = 0,
    IntensityStereo = 1,
    MsStereo = 2,
};
AK_ENUM_BITWISE_OPERATORS(ModeExtension)

enum class Emphasis {
    None = 0,
    Microseconds_50_15 = 1,
    Reserved = 2,
    CCITT_J17 = 3,
};

enum class BlockType {
    Normal = 0,
    Start = 1,
    Short = 2,
    End = 3,
};

struct Header {
    i32 id { 0 };
    i32 layer { 0 };
    bool protection_bit { false };
    i32 bitrate { 0 };
    i32 samplerate { 0 };
    bool padding_bit { false };
    bool private_bit { false };
    Mode mode { Mode::Stereo };
    ModeExtension mode_extension { ModeExtension::Stereo };
    bool copyright_bit { false };
    bool original_bit { false };
    Emphasis emphasis { Emphasis::None };
    u16 crc16 { 0 };
    size_t header_size { 0 };
    size_t frame_size { 0 };
    size_t slot_count { 0 };

    size_t channel_count() const { return mode == Mode::SingleChannel ? 1 : 2; }
};

struct Granule {
    Array<float, MP3::granule_size> samples;
    Array<Array<float, 18>, 32> filter_bank_input;
    Array<Array<float, 32>, 18> pcm;
    u32 part_2_3_length { 0 };
    u32 big_values { 0 };
    u32 global_gain { 0 };
    u32 scalefac_compress { 0 };
    bool window_switching_flag { false };
    BlockType block_type { BlockType::Normal };
    bool mixed_block_flag { false };
    Array<int, 3> table_select;
    Array<int, 3> sub_block_gain;
    u32 region0_count { 0 };
    u32 region1_count { 0 };
    bool preflag { false };
    bool scalefac_scale { false };
    bool count1table_select { false };
};

struct Channel {
    Array<Granule, 2> granules;
    Array<int, 39> scale_factors;
    Array<int, 4> scale_factor_selection_info;
};

struct MP3Frame {
    Header header;
    FixedArray<Channel> channels;
    off_t main_data_begin { 0 };
    u32 private_bits { 0 };

    MP3Frame(Header header)
        : header(header)
        , channels(FixedArray<Channel>::must_create_but_fixme_should_propagate_errors(header.channel_count()))
    {
    }
};

}
