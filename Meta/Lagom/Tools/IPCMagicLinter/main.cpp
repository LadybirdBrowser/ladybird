/*
 * Copyright (c) 2021, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>

// Exit code is bitwise-or of these values:
static constexpr auto EXIT_COLLISION = 0x1;
static constexpr auto EXIT_ERROR = 0x2;

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    if (arguments.argc < 3) {
        warnln("Usage: {} path/to/some.ipc path/to/other.ipc [more ipc files ...]", arguments.strings[0]);
        return EXIT_ERROR;
    }

    // Read files, compute their hashes, ignore collisions for now.
    HashMap<u32, Vector<ByteString>> inverse_hashes;
    bool had_errors = false;
    for (auto filename : arguments.strings.slice(1)) {

        auto const open_file = [](StringView filename) -> ErrorOr<NonnullOwnPtr<Core::InputBufferedFile>> {
            auto file = TRY(Core::File::open(filename, Core::File::OpenMode::Read));
            return Core::InputBufferedFile::create(move(file));
        };

        auto file_or_error = open_file(filename);

        if (file_or_error.is_error()) {
            warnln("Error: Cannot open '{}': {}", filename, file_or_error.error());
            had_errors = true;
            continue; // next file
        }

        auto file = file_or_error.release_value();

        ByteString endpoint_name;

        auto const read_lines = [&]() -> ErrorOr<void> {
            while (TRY(file->can_read_line())) {
                Array<u8, 1024> buffer;
                auto line = TRY(file->read_line(buffer));

                if (!line.starts_with("endpoint "sv))
                    continue;
                auto line_endpoint_name = line.substring_view("endpoint "sv.length());
                if (!endpoint_name.is_empty()) {
                    // Note: If there are three or more endpoints defined in a file, these errors will look a bit wonky.
                    // However, that's fine, because it shouldn't happen in the first place.
                    warnln("Error: Multiple endpoints in file '{}': Found {} and {}", filename, endpoint_name, line_endpoint_name);
                    had_errors = true;
                    continue; // next line
                }
                endpoint_name = line_endpoint_name;
            }

            return {};
        };

        auto maybe_error = read_lines();

        if (maybe_error.is_error()) {
            warnln("Error: Failed to read '{}': {}", filename, maybe_error.release_error());
            had_errors = true;
            continue; // next file
        }
        if (endpoint_name.is_empty()) {
            // If this happens, this file probably needs to parse the endpoint name more carefully.
            warnln("Error: Could not detect endpoint name in file '{}'", filename);
            had_errors = true;
            continue; // next file
        }
        u32 hash = endpoint_name.hash();
        auto& files_with_hash = inverse_hashes.ensure(hash);
        files_with_hash.append(filename);
    }

    // Report any collisions
    bool had_collisions = false;
    for (auto const& specific_collisions : inverse_hashes) {
        if (specific_collisions.value.size() <= 1)
            continue;
        outln("Collision: Multiple endpoints use the magic number {}:", specific_collisions.key);
        for (auto const& colliding_file : specific_collisions.value) {
            outln("- {}", colliding_file);
        }
        had_collisions = true;
    }

    outln("Checked {} files, saw {} distinct magic numbers.", arguments.argc - 1, inverse_hashes.size());
    if (had_collisions)
        outln("Consider giving your new service a different name.");

    if (had_errors)
        warnln("Some errors were encountered. There may be endpoints with colliding magic numbers.");

    int exit_code = 0;
    if (had_collisions)
        exit_code |= EXIT_COLLISION;
    if (had_errors)
        exit_code |= EXIT_ERROR;
    return exit_code;
}
