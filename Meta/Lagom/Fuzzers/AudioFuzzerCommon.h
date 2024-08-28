/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2021-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/MemoryStream.h>
#include <LibMedia/Audio/Loader.h>
#include <stddef.h>
#include <stdint.h>

template<typename LoaderPluginType>
requires(IsBaseOf<Audio::LoaderPlugin, LoaderPluginType>)
int fuzz_audio_loader(uint8_t const* data, size_t size)
{
    auto const bytes = ReadonlyBytes { data, size };
    auto stream = try_make<FixedMemoryStream>(bytes).release_value();
    auto audio_or_error = LoaderPluginType::create(move(stream));

    if (audio_or_error.is_error())
        return 0;

    auto audio = audio_or_error.release_value();

    for (;;) {
        auto samples = audio->load_chunks(4 * KiB);
        if (samples.is_error())
            return 0;
        if (samples.value().size() == 0)
            break;
    }

    return 0;
}
