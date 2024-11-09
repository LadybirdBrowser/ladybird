/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Archive {

class Statistics {
public:
    Statistics(size_t file_count, size_t directory_count, size_t total_uncompressed_bytes)
        : m_file_count(file_count)
        , m_directory_count(directory_count)
        , m_total_uncompressed_bytes(total_uncompressed_bytes)
    {
    }

    size_t file_count() const { return m_file_count; }
    size_t directory_count() const { return m_directory_count; }
    size_t member_count() const { return file_count() + directory_count(); }
    size_t total_uncompressed_bytes() const { return m_total_uncompressed_bytes; }

private:
    size_t m_file_count { 0 };
    size_t m_directory_count { 0 };
    size_t m_total_uncompressed_bytes { 0 };
};

}
