/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibCompress/Gzip.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>
#include <unistd.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    Vector<StringView> filenames;
    bool keep_input_files { false };
    bool write_to_stdout { false };
    bool decompress { false };

    Core::ArgsParser args_parser;
    args_parser.add_option(keep_input_files, "Keep (don't delete) input files", "keep", 'k');
    args_parser.add_option(write_to_stdout, "Write to stdout, keep original files unchanged", "stdout", 'c');
    args_parser.add_option(decompress, "Decompress", "decompress", 'd');
    args_parser.add_positional_argument(filenames, "Files", "FILES", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    auto program_name = LexicalPath::basename(arguments.strings[0]);

    // NOTE: If the user run this program via the /bin/zcat or /bin/gunzip symlink,
    // then emulate gzip decompression.
    if (program_name == "zcat"sv || program_name == "gunzip"sv)
        decompress = true;

    if (program_name == "zcat"sv)
        write_to_stdout = true;

    if (filenames.is_empty()) {
        filenames.append("-"sv);
        write_to_stdout = true;
    }

    if (write_to_stdout)
        keep_input_files = true;

    for (auto const& input_filename : filenames) {
        OwnPtr<Stream> output_stream;

        if (write_to_stdout) {
            output_stream = TRY(Core::File::standard_output());
        } else if (decompress) {
            if (!input_filename.ends_with(".gz"sv)) {
                warnln("unknown suffix for: {}, skipping", input_filename);
                continue;
            }

            auto output_filename = input_filename.substring_view(0, input_filename.length() - ".gz"sv.length());
            output_stream = TRY(Core::File::open(output_filename, Core::File::OpenMode::Write));
        } else {
            auto output_filename = ByteString::formatted("{}.gz", input_filename);
            output_stream = TRY(Core::File::open(output_filename, Core::File::OpenMode::Write));
        }

        VERIFY(output_stream);

        NonnullOwnPtr<Core::File> input_file = TRY(Core::File::open_file_or_standard_stream(input_filename, Core::File::OpenMode::Read));

        // Buffer reads, which yields a significant performance improvement.
        NonnullOwnPtr<Stream> input_stream = TRY(Core::InputBufferedFile::create(move(input_file), 1 * MiB));

        if (decompress) {
            input_stream = TRY(try_make<Compress::GzipDecompressor>(move(input_stream)));
        } else {
            output_stream = TRY(try_make<Compress::GzipCompressor>(output_stream.release_nonnull()));
        }

        auto buffer = TRY(ByteBuffer::create_uninitialized(1 * MiB));

        while (!input_stream->is_eof()) {
            auto span = TRY(input_stream->read_some(buffer));
            TRY(output_stream->write_until_depleted(span));
        }

        if (!keep_input_files)
            TRY(Core::System::unlink(input_filename));
    }

    return 0;
}
