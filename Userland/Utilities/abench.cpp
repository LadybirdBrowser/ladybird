/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/Types.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ElapsedTimer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>
#include <LibMedia/Audio/Loader.h>
#include <stdio.h>

// The Kernel has problems with large anonymous buffers, so let's limit sample reads ourselves.
static constexpr size_t MAX_CHUNK_SIZE = 1 * MiB / 2;

ErrorOr<int> serenity_main(Main::Arguments args)
{
    StringView path {};
    int sample_count = -1;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("Benchmark audio loading");
    args_parser.add_positional_argument(path, "Path to audio file", "path");
    args_parser.add_option(sample_count, "How many samples to load at maximum", "sample-count", 's', "samples");
    args_parser.parse(args);

    auto maybe_loader = Audio::Loader::create(path);
    if (maybe_loader.is_error()) {
        warnln("Failed to load audio file: {}", maybe_loader.error().description);
        return 1;
    }
    auto loader = maybe_loader.release_value();

    Core::ElapsedTimer sample_timer { Core::TimerType::Precise };
    i64 total_loader_time = 0;
    int remaining_samples = sample_count > 0 ? sample_count : NumericLimits<int>::max();
    unsigned total_loaded_samples = 0;

    for (;;) {
        if (remaining_samples > 0) {
            sample_timer = sample_timer.start_new();
            auto samples = loader->get_more_samples(min(MAX_CHUNK_SIZE, remaining_samples));
            total_loader_time += sample_timer.elapsed_milliseconds();
            if (!samples.is_error()) {
                remaining_samples -= samples.value().size();
                total_loaded_samples += samples.value().size();
                if (samples.value().size() == 0)
                    break;
            } else {
                warnln("Error while loading audio: {}", samples.error().description);
                return 1;
            }
        } else
            break;
    }

    auto time_per_sample = static_cast<double>(total_loader_time) / static_cast<double>(total_loaded_samples) * 1000.;
    auto playback_time_per_sample = (1. / static_cast<double>(loader->sample_rate())) * 1000'000.;

    outln("Loaded {:10d} samples in {:06.3f} s, {:9.3f} µs/sample, {:6.1f}% speed (realtime {:9.3f} µs/sample)", total_loaded_samples, static_cast<double>(total_loader_time) / 1000., time_per_sample, playback_time_per_sample / time_per_sample * 100., playback_time_per_sample);

    return 0;
}
