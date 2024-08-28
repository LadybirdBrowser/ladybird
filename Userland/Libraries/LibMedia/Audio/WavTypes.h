/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Audio::Wav {

static constexpr StringView const wave_subformat_id = "WAVE"sv;
static constexpr StringView const data_chunk_id = "data"sv;
static constexpr StringView const info_chunk_id = "INFO"sv;
static constexpr StringView const format_chunk_id = "fmt "sv;

// Constants for handling WAVE header data.
enum class WaveFormat : u32 {
    Pcm = 0x0001,        // WAVE_FORMAT_PCM
    IEEEFloat = 0x0003,  // WAVE_FORMAT_IEEE_FLOAT
    ALaw = 0x0006,       // 8-bit ITU-T G.711 A-law
    MuLaw = 0x0007,      // 8-bit ITU-T G.711 µ-law
    Extensible = 0xFFFE, // Determined by SubFormat
};

}
