/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Xz.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView filename;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("Decompress and print an XZ archive");
    args_parser.add_positional_argument(filename, "File to decompress", "file");
    args_parser.parse(arguments);

    auto file = TRY(Core::File::open_file_or_standard_stream(filename, Core::File::OpenMode::Read));
    auto buffered_file = TRY(Core::InputBufferedFile::create(move(file)));
    auto stream = TRY(Compress::XzDecompressor::create(move(buffered_file)));

    // Arbitrarily chosen buffer size.
    Array<u8, 4096> buffer;
    while (!stream->is_eof()) {
        auto slice = TRY(stream->read_some(buffer));
        out("{:s}", slice);
    }

    return 0;
}
