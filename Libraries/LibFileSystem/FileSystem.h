/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/StringView.h>
#include <LibCore/File.h>
#include <sys/stat.h>

namespace FileSystem {

#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin"
#define DEFAULT_PATH_SV "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin"sv

ErrorOr<ByteString> current_working_directory();
ErrorOr<ByteString> absolute_path(StringView path);
ErrorOr<ByteString> real_path(StringView path);

bool exists(StringView path);
bool exists(int fd);

bool is_regular_file(StringView path);
bool is_regular_file(int fd);

bool is_directory(StringView path);
bool is_directory(int fd);

bool is_link(StringView path);
bool is_link(int fd);

enum class RecursionMode {
    Allowed,
    Disallowed
};

enum class LinkMode {
    Allowed,
    Disallowed
};

enum class AddDuplicateFileMarker {
    Yes,
    No,
};

enum class PreserveMode {
    Nothing = 0,
    Permissions = (1 << 0),
    Ownership = (1 << 1),
    Timestamps = (1 << 2),
};
AK_ENUM_BITWISE_OPERATORS(PreserveMode);

ErrorOr<void> copy_file(StringView destination_path, StringView source_path, struct stat const& source_stat, Core::File& source, PreserveMode = PreserveMode::Nothing);
ErrorOr<void> copy_directory(StringView destination_path, StringView source_path, struct stat const& source_stat, LinkMode = LinkMode::Disallowed, PreserveMode = PreserveMode::Nothing);
ErrorOr<void> copy_file_or_directory(StringView destination_path, StringView source_path, RecursionMode = RecursionMode::Allowed, LinkMode = LinkMode::Disallowed, AddDuplicateFileMarker = AddDuplicateFileMarker::Yes, PreserveMode = PreserveMode::Nothing);
ErrorOr<void> move_file(StringView destination_path, StringView source_path, PreserveMode = PreserveMode::Nothing);
ErrorOr<void> remove(StringView path, RecursionMode);
ErrorOr<off_t> size_from_stat(StringView path);
ErrorOr<off_t> size_from_fstat(int fd);
bool can_delete_or_move(StringView path);

}
