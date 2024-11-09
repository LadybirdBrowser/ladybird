/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/DirIterator.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

namespace Test {

inline double get_time_in_ms()
{
    struct timeval tv1;
    auto return_code = gettimeofday(&tv1, nullptr);
    VERIFY(return_code >= 0);
    return static_cast<double>(tv1.tv_sec) * 1000.0 + static_cast<double>(tv1.tv_usec) / 1000.0;
}

template<typename Callback>
inline void iterate_directory_recursively(ByteString const& directory_path, Callback callback)
{
    Core::DirIterator directory_iterator(directory_path, Core::DirIterator::Flags::SkipDots);

    while (directory_iterator.has_next()) {
        auto name = directory_iterator.next_path();
        struct stat st = {};
        if (fstatat(directory_iterator.fd(), name.characters(), &st, AT_SYMLINK_NOFOLLOW) < 0)
            continue;
        bool is_directory = S_ISDIR(st.st_mode);
        auto full_path = ByteString::formatted("{}/{}", directory_path, name);
        if (is_directory && name != "/Fixtures"sv) {
            iterate_directory_recursively(full_path, callback);
        } else if (!is_directory) {
            callback(full_path);
        }
    }
}

}
