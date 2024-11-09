/*
 * Copyright (c) 2020, Peter Elliott <pelliott@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Tar.h"

namespace Archive {

unsigned TarFileHeader::expected_checksum() const
{
    auto checksum = 0u;
    u8 const* u8_this = reinterpret_cast<u8 const*>(this);
    u8 const* u8_m_checksum = reinterpret_cast<u8 const*>(&m_checksum);
    for (auto i = 0u; i < sizeof(TarFileHeader); ++i) {
        if (u8_this + i >= u8_m_checksum && u8_this + i < u8_m_checksum + sizeof(m_checksum)) {
            checksum += ' ';
        } else {
            checksum += u8_this[i];
        }
    }
    return checksum;
}

ErrorOr<void> TarFileHeader::calculate_checksum()
{
    memset(m_checksum, ' ', sizeof(m_checksum));

    auto octal = TRY(String::formatted("{:06o}", expected_checksum()));
    bool copy_successful = octal.bytes_as_string_view().copy_characters_to_buffer(m_checksum, sizeof(m_checksum));
    VERIFY(copy_successful);

    return {};
}

bool TarFileHeader::is_zero_block() const
{
    u8 const* buffer = reinterpret_cast<u8 const*>(this);
    for (size_t i = 0; i < sizeof(TarFileHeader); ++i) {
        if (buffer[i] != 0)
            return false;
    }
    return true;
}

bool TarFileHeader::content_is_like_extended_header() const
{
    return type_flag() == TarFileType::ExtendedHeader || type_flag() == TarFileType::GlobalExtendedHeader;
}

void TarFileHeader::set_filename_and_prefix(StringView filename)
{
    // FIXME: Add support for extended tar headers for longer filenames.
    VERIFY(filename.length() <= sizeof(m_filename) + sizeof(m_prefix));

    if (filename.length() <= sizeof(m_filename)) {
        set_prefix(""sv);
        set_filename(filename);
        return;
    }

    Optional<size_t> slash = filename.find('/', filename.length() - sizeof(m_filename));

    VERIFY(slash.has_value());
    set_prefix(filename.substring_view(0, slash.value() + 1));
    set_filename(filename.substring_view(slash.value() + 1));
}

}
