/*
 * Copyright (c) 2020-2022, Peter Elliott <pelliott@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <string.h>
#include <sys/types.h>

// glibc before 2.28 defines these from sys/types.h, but we don't want
// TarFileHeader::major() and TarFileHeader::minor() to use those macros
#ifdef minor
#    undef minor
#endif

#ifdef major
#    undef major
#endif

namespace Archive {

enum class TarFileType : char {
    NormalFile = '0',
    AlternateNormalFile = '\0',
    HardLink = '1',
    SymLink = '2',
    CharacterSpecialFile = '3',
    BlockSpecialFile = '4',
    Directory = '5',
    FIFO = '6',
    ContiguousFile = '7',
    GlobalExtendedHeader = 'g',
    ExtendedHeader = 'x',

    // GNU extensions
    LongName = 'L',
};

constexpr size_t block_size = 512;
constexpr StringView gnu_magic = "ustar "sv;    // gnu format magic
constexpr StringView gnu_version = " "sv;       // gnu format version
constexpr StringView ustar_magic = "ustar"sv;   // ustar format magic
constexpr StringView ustar_version = "00"sv;    // ustar format version
constexpr StringView posix1_tar_magic = ""sv;   // POSIX.1-1988 format magic
constexpr StringView posix1_tar_version = ""sv; // POSIX.1-1988 format version

template<size_t N>
static ErrorOr<size_t> get_field_as_integral(char const (&field)[N])
{
    size_t value = 0;
    for (size_t i = 0; i < N; ++i) {
        if (field[i] == 0 || field[i] == ' ')
            break;

        if (field[i] < '0' || field[i] > '7')
            return Error::from_string_literal("Passed a non-octal value");
        value *= 8;
        value += field[i] - '0';
    }
    return value;
}

template<size_t N>
static StringView get_field_as_string_view(char const (&field)[N])
{
    return { field, min(__builtin_strlen(field), N) };
}

template<size_t N, class TSource>
static void set_field(char (&field)[N], TSource&& source)
{
    VERIFY(N >= source.length());
    memcpy(field, StringView(source).characters_without_null_termination(), source.length());
    if (N > source.length())
        field[source.length()] = 0;
}

template<class TSource, size_t N>
static ErrorOr<void> set_octal_field(char (&field)[N], TSource&& source)
{
    auto octal = TRY(String::formatted("{:o}", forward<TSource>(source)));
    set_field(field, octal.bytes_as_string_view());
    return {};
}

class [[gnu::packed]] TarFileHeader {
public:
    StringView filename() const { return get_field_as_string_view(m_filename); }
    ErrorOr<mode_t> mode() const { return TRY(get_field_as_integral(m_mode)); }
    ErrorOr<uid_t> uid() const { return TRY(get_field_as_integral(m_uid)); }
    ErrorOr<gid_t> gid() const { return TRY(get_field_as_integral(m_gid)); }
    // FIXME: support 2001-star size encoding
    ErrorOr<size_t> size() const { return TRY(get_field_as_integral(m_size)); }
    ErrorOr<time_t> timestamp() const { return TRY(get_field_as_integral(m_timestamp)); }
    ErrorOr<unsigned> checksum() const { return TRY(get_field_as_integral(m_checksum)); }
    TarFileType type_flag() const { return TarFileType(m_type_flag); }
    StringView link_name() const { return { m_link_name, strlen(m_link_name) }; }
    StringView magic() const { return get_field_as_string_view(m_magic); }
    StringView version() const { return get_field_as_string_view(m_version); }
    StringView owner_name() const { return get_field_as_string_view(m_owner_name); }
    StringView group_name() const { return get_field_as_string_view(m_group_name); }
    ErrorOr<int> major() const { return TRY(get_field_as_integral(m_major)); }
    ErrorOr<int> minor() const { return TRY(get_field_as_integral(m_minor)); }
    // FIXME: support ustar filename prefix
    StringView prefix() const { return get_field_as_string_view(m_prefix); }

    void set_filename(StringView filename) { set_field(m_filename, filename); }
    ErrorOr<void> set_mode(mode_t mode) { return set_octal_field(m_mode, mode); }
    ErrorOr<void> set_uid(uid_t uid) { return set_octal_field(m_uid, uid); }
    ErrorOr<void> set_gid(gid_t gid) { return set_octal_field(m_gid, gid); }
    ErrorOr<void> set_size(size_t size) { return set_octal_field(m_size, size); }
    ErrorOr<void> set_timestamp(time_t timestamp) { return set_octal_field(m_timestamp, timestamp); }
    void set_type_flag(TarFileType type) { m_type_flag = to_underlying(type); }
    void set_link_name(StringView link_name) { set_field(m_link_name, link_name); }
    // magic doesn't necessarily include a null byte
    void set_magic(StringView magic) { set_field(m_magic, magic); }
    // version doesn't necessarily include a null byte
    void set_version(StringView version) { set_field(m_version, version); }
    void set_owner_name(StringView owner_name) { set_field(m_owner_name, owner_name); }
    void set_group_name(StringView group_name) { set_field(m_group_name, group_name); }
    ErrorOr<void> set_major(int major) { return set_octal_field(m_major, major); }
    ErrorOr<void> set_minor(int minor) { return set_octal_field(m_minor, minor); }
    void set_prefix(StringView prefix) { set_field(m_prefix, prefix); }

    unsigned expected_checksum() const;
    ErrorOr<void> calculate_checksum();

    bool is_zero_block() const;
    bool content_is_like_extended_header() const;

    void set_filename_and_prefix(StringView filename);

private:
    char m_filename[100] { 0 };
    char m_mode[8] { 0 };
    char m_uid[8] { 0 };
    char m_gid[8] { 0 };
    char m_size[12] { 0 };
    char m_timestamp[12] { 0 };
    char m_checksum[8] { 0 }; // an uninitialized header's checksum is filled with spaces
    char m_type_flag { 0 };
    char m_link_name[100] { 0 };
    char m_magic[6] { 0 };
    char m_version[2] { 0 };
    char m_owner_name[32] { 0 };
    char m_group_name[32] { 0 };
    char m_major[8] { 0 };
    char m_minor[8] { 0 };
    char m_prefix[155] { 0 }; // zero out the prefix for archiving
};

}

template<>
struct AK::Traits<Archive::TarFileHeader> : public AK::DefaultTraits<Archive::TarFileHeader> {
    static constexpr bool is_trivially_serializable() { return true; }
};
