/*
 * Copyright (c) 2020, Peter Elliott <pelliott@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibArchive/TarStream.h>
#include <LibCompress/Gzip.h>
#include <LibCompress/Lzma.h>
#include <LibCompress/Xz.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr size_t buffer_size = 4096;

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    bool create = false;
    bool extract = false;
    bool list = false;
    bool verbose = false;
    bool gzip = false;
    bool lzma = false;
    bool xz = false;
    bool no_auto_compress = false;
    StringView archive_file;
    bool dereference = false;
    StringView directory;
    Vector<ByteString> paths;

    Core::ArgsParser args_parser;
    args_parser.add_option(create, "Create archive", "create", 'c');
    args_parser.add_option(extract, "Extract archive", "extract", 'x');
    args_parser.add_option(list, "List contents", "list", 't');
    args_parser.add_option(verbose, "Print paths", "verbose", 'v');
    args_parser.add_option(gzip, "Compress or decompress file using gzip", "gzip", 'z');
    args_parser.add_option(lzma, "Compress or decompress file using lzma", "lzma");
    args_parser.add_option(xz, "Compress or decompress file using xz", "xz", 'J');
    args_parser.add_option(no_auto_compress, "Do not use the archive suffix to select the compression algorithm", "no-auto-compress");
    args_parser.add_option(directory, "Directory to extract to/create from", "directory", 'C', "DIRECTORY");
    args_parser.add_option(archive_file, "Archive file", "file", 'f', "FILE");
    args_parser.add_option(dereference, "Follow symlinks", "dereference", 'h');
    args_parser.add_positional_argument(paths, "Paths", "PATHS", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    if (create + extract + list != 1) {
        warnln("exactly one of -c, -x, and -t can be used");
        return 1;
    }

    if (!no_auto_compress && !archive_file.is_empty()) {
        if (archive_file.ends_with(".gz"sv) || archive_file.ends_with(".tgz"sv))
            gzip = true;
        if (archive_file.ends_with(".lzma"sv))
            lzma = true;
        if (archive_file.ends_with(".xz"sv))
            xz = true;
    }

    if (list || extract) {
        NonnullOwnPtr<Stream> input_stream = TRY(Core::InputBufferedFile::create(TRY(Core::File::open_file_or_standard_stream(archive_file, Core::File::OpenMode::Read))));

        if (!directory.is_empty())
            TRY(Core::System::chdir(directory));

        if (gzip)
            input_stream = make<Compress::GzipDecompressor>(move(input_stream));

        if (lzma)
            input_stream = TRY(Compress::LzmaDecompressor::create_from_container(move(input_stream)));

        if (xz)
            input_stream = TRY(Compress::XzDecompressor::create(move(input_stream)));

        auto tar_stream = TRY(Archive::TarInputStream::construct(move(input_stream)));

        HashMap<ByteString, ByteString> global_overrides;
        HashMap<ByteString, ByteString> local_overrides;

        auto get_override = [&](StringView key) -> Optional<ByteString> {
            Optional<ByteString> maybe_local = local_overrides.get(key);

            if (maybe_local.has_value())
                return maybe_local;

            Optional<ByteString> maybe_global = global_overrides.get(key);

            if (maybe_global.has_value())
                return maybe_global;

            return {};
        };

        while (!tar_stream->finished()) {
            Archive::TarFileHeader const& header = tar_stream->header();

            // Handle meta-entries earlier to avoid consuming the file content stream.
            if (header.content_is_like_extended_header()) {
                switch (header.type_flag()) {
                case Archive::TarFileType::GlobalExtendedHeader: {
                    TRY(tar_stream->for_each_extended_header([&](StringView key, StringView value) {
                        if (value.length() == 0)
                            global_overrides.remove(key);
                        else
                            global_overrides.set(key, value);
                    }));
                    break;
                }
                case Archive::TarFileType::ExtendedHeader: {
                    TRY(tar_stream->for_each_extended_header([&](StringView key, StringView value) {
                        local_overrides.set(key, value);
                    }));
                    break;
                }
                default:
                    warnln("Unknown extended header type '{}' of {}", (char)header.type_flag(), header.filename());
                    VERIFY_NOT_REACHED();
                }

                TRY(tar_stream->advance());
                continue;
            }

            Archive::TarFileStream file_stream = tar_stream->file_contents();

            // Handle other header types that don't just have an effect on extraction.
            switch (header.type_flag()) {
            case Archive::TarFileType::LongName: {
                StringBuilder long_name;

                Array<u8, buffer_size> buffer;

                while (!file_stream.is_eof()) {
                    auto slice = TRY(file_stream.read_some(buffer));
                    long_name.append(reinterpret_cast<char*>(slice.data()), slice.size());
                }

                local_overrides.set("path", long_name.to_byte_string());
                TRY(tar_stream->advance());
                continue;
            }
            default:
                // None of the relevant headers, so continue as normal.
                break;
            }

            LexicalPath path = LexicalPath(header.filename());
            if (!header.prefix().is_empty())
                path = path.prepend(header.prefix());
            ByteString filename = get_override("path"sv).value_or(path.string());

            if (list || verbose)
                outln("{}", filename);

            if (extract) {
                auto absolute_path = TRY(FileSystem::absolute_path(filename));
                auto parent_path = LexicalPath(absolute_path).parent();
                auto header_mode = TRY(header.mode());

                switch (header.type_flag()) {
                case Archive::TarFileType::NormalFile:
                case Archive::TarFileType::AlternateNormalFile: {
                    MUST(Core::Directory::create(parent_path, Core::Directory::CreateDirectories::Yes));

                    int fd = TRY(Core::System::open(absolute_path, O_CREAT | O_WRONLY, header_mode));

                    Array<u8, buffer_size> buffer;
                    while (!file_stream.is_eof()) {
                        auto slice = TRY(file_stream.read_some(buffer));
                        TRY(Core::System::write(fd, slice));
                    }

                    TRY(Core::System::close(fd));
                    break;
                }
                case Archive::TarFileType::SymLink: {
                    MUST(Core::Directory::create(parent_path, Core::Directory::CreateDirectories::Yes));

                    TRY(Core::System::symlink(header.link_name(), absolute_path));
                    break;
                }
                case Archive::TarFileType::Directory: {
                    MUST(Core::Directory::create(parent_path, Core::Directory::CreateDirectories::Yes));

                    auto result_or_error = Core::System::mkdir(absolute_path, header_mode);
                    if (result_or_error.is_error() && result_or_error.error().code() != EEXIST)
                        return result_or_error.release_error();
                    break;
                }
                default:
                    // FIXME: Implement other file types
                    warnln("file type '{}' of {} is not yet supported", (char)header.type_flag(), header.filename());
                    VERIFY_NOT_REACHED();
                }
            }

            // Non-global headers should be cleared after every file.
            local_overrides.clear();

            TRY(tar_stream->advance());
        }

        return 0;
    }

    if (create) {
        if (paths.size() == 0) {
            warnln("you must provide at least one path to be archived");
            return 1;
        }

        NonnullOwnPtr<Stream> output_stream = TRY(Core::File::standard_output());

        if (!archive_file.is_empty())
            output_stream = TRY(Core::File::open(archive_file, Core::File::OpenMode::Write));

        if (!directory.is_empty())
            TRY(Core::System::chdir(directory));

        if (gzip)
            output_stream = TRY(try_make<Compress::GzipCompressor>(move(output_stream)));

        if (lzma)
            output_stream = TRY(Compress::LzmaCompressor::create_container(move(output_stream), {}));

        if (xz)
            TODO();

        Archive::TarOutputStream tar_stream(move(output_stream));

        auto add_file = [&](ByteString path) -> ErrorOr<void> {
            auto file_or_error = Core::File::open(path, Core::File::OpenMode::Read);
            if (file_or_error.is_error()) {
                warnln("Failed to open {}: {}", path, file_or_error.error());
                return {};
            }
            auto file = file_or_error.release_value();

            auto statbuf = TRY(Core::System::lstat(path));
            auto canonicalized_path = LexicalPath::canonicalized_path(path);
            // FIXME: We should stream instead of reading the entire file in one go, but TarOutputStream does not have any interface to do so.
            auto file_content = TRY(file->read_until_eof());
            TRY(tar_stream.add_file(canonicalized_path, statbuf.st_mode, file_content));
            if (verbose)
                outln("{}", canonicalized_path);

            return {};
        };

        auto add_link = [&](ByteString path) -> ErrorOr<void> {
            auto statbuf = TRY(Core::System::lstat(path));

            auto canonicalized_path = LexicalPath::canonicalized_path(path);
            TRY(tar_stream.add_link(canonicalized_path, statbuf.st_mode, TRY(Core::System::readlink(path))));
            if (verbose)
                outln("{}", canonicalized_path);

            return {};
        };

        auto add_directory = [&](ByteString path, auto handle_directory) -> ErrorOr<void> {
            auto statbuf = TRY(Core::System::lstat(path));

            auto canonicalized_path = LexicalPath::canonicalized_path(path);
            TRY(tar_stream.add_directory(canonicalized_path, statbuf.st_mode));
            if (verbose)
                outln("{}", canonicalized_path);

            Core::DirIterator it(path, Core::DirIterator::Flags::SkipParentAndBaseDir);
            while (it.has_next()) {
                auto child_path = it.next_full_path();
                if (!dereference && FileSystem::is_link(child_path)) {
                    TRY(add_link(child_path));
                } else if (!FileSystem::is_directory(child_path)) {
                    TRY(add_file(child_path));
                } else {
                    TRY(handle_directory(child_path, handle_directory));
                }
            }

            return {};
        };

        for (auto const& path : paths) {
            if (FileSystem::is_directory(path)) {
                TRY(add_directory(path, add_directory));
            } else {
                TRY(add_file(path));
            }
        }

        TRY(tar_stream.finish());

        return 0;
    }

    return 0;
}
