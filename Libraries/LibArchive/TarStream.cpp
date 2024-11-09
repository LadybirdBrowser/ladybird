/*
 * Copyright (c) 2020, Peter Elliott <pelliott@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/OwnPtr.h>
#include <LibArchive/TarStream.h>
#include <string.h>

namespace Archive {
TarFileStream::TarFileStream(TarInputStream& tar_stream)
    : m_tar_stream(tar_stream)
    , m_generation(tar_stream.m_generation)
{
}

ErrorOr<Bytes> TarFileStream::read_some(Bytes bytes)
{
    // Verify that the stream has not advanced.
    VERIFY(m_tar_stream.m_generation == m_generation);

    auto header_size = TRY(m_tar_stream.header().size());

    auto to_read = min(bytes.size(), header_size - m_tar_stream.m_file_offset);

    auto slice = TRY(m_tar_stream.m_stream->read_some(bytes.trim(to_read)));
    m_tar_stream.m_file_offset += slice.size();

    return slice;
}

bool TarFileStream::is_eof() const
{
    // Verify that the stream has not advanced.
    VERIFY(m_tar_stream.m_generation == m_generation);

    auto header_size_or_error = m_tar_stream.header().size();
    if (header_size_or_error.is_error())
        return true;
    auto header_size = header_size_or_error.release_value();

    return m_tar_stream.m_stream->is_eof()
        || m_tar_stream.m_file_offset >= header_size;
}

ErrorOr<size_t> TarFileStream::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<NonnullOwnPtr<TarInputStream>> TarInputStream::construct(NonnullOwnPtr<Stream> stream)
{
    auto tar_stream = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TarInputStream(move(stream))));

    TRY(tar_stream->load_next_header());

    return tar_stream;
}

TarInputStream::TarInputStream(NonnullOwnPtr<Stream> stream)
    : m_stream(move(stream))
{
}

static constexpr unsigned long block_ceiling(unsigned long offset)
{
    return block_size * (1 + ((offset - 1) / block_size));
}

ErrorOr<void> TarInputStream::advance()
{
    if (finished())
        return Error::from_string_literal("Attempted to advance a finished stream");

    m_generation++;

    // Discard the pending bytes of the current entry.
    auto file_size = TRY(m_header.size());
    TRY(m_stream->discard(block_ceiling(file_size) - m_file_offset));
    m_file_offset = 0;

    TRY(load_next_header());

    return {};
}

ErrorOr<void> TarInputStream::load_next_header()
{
    size_t number_of_consecutive_zero_blocks = 0;
    while (true) {
        m_header = TRY(m_stream->read_value<TarFileHeader>());

        // Discard the rest of the header block.
        TRY(m_stream->discard(block_size - sizeof(TarFileHeader)));

        if (!header().is_zero_block())
            break;

        number_of_consecutive_zero_blocks++;

        // Two zero blocks in a row marks the end of the archive.
        if (number_of_consecutive_zero_blocks >= 2) {
            m_found_end_of_archive = true;
            return {};
        }
    }

    if (!TRY(valid()))
        return Error::from_string_literal("Header has an invalid magic or checksum");

    return {};
}

ErrorOr<bool> TarInputStream::valid() const
{
    auto const header_magic = header().magic();
    auto const header_version = header().version();

    if (!((header_magic == gnu_magic && header_version == gnu_version)
            || (header_magic == ustar_magic && header_version == ustar_version)
            || (header_magic == posix1_tar_magic && header_version == posix1_tar_version)))
        return false;

    // POSIX.1-1988 tar does not have magic numbers, so we also need to verify the header checksum.
    return TRY(header().checksum()) == header().expected_checksum();
}

TarFileStream TarInputStream::file_contents()
{
    VERIFY(!finished());
    return TarFileStream(*this);
}

TarOutputStream::TarOutputStream(MaybeOwned<Stream> stream)
    : m_stream(move(stream))
{
}

ErrorOr<void> TarOutputStream::add_directory(StringView path, mode_t mode)
{
    VERIFY(!m_finished);
    TarFileHeader header {};
    TRY(header.set_size(0));
    header.set_filename_and_prefix(TRY(String::formatted("{}/", path))); // Old tar implementations assume directory names end with a /
    header.set_type_flag(TarFileType::Directory);
    TRY(header.set_mode(mode));
    header.set_magic(gnu_magic);
    header.set_version(gnu_version);
    TRY(header.calculate_checksum());
    TRY(m_stream->write_until_depleted(Bytes { &header, sizeof(header) }));
    u8 padding[block_size] = { 0 };
    TRY(m_stream->write_until_depleted(Bytes { &padding, block_size - sizeof(header) }));
    return {};
}

ErrorOr<void> TarOutputStream::add_file(StringView path, mode_t mode, ReadonlyBytes bytes)
{
    VERIFY(!m_finished);
    TarFileHeader header {};
    TRY(header.set_size(bytes.size()));
    header.set_filename_and_prefix(path);
    header.set_type_flag(TarFileType::NormalFile);
    TRY(header.set_mode(mode));
    header.set_magic(gnu_magic);
    header.set_version(gnu_version);
    TRY(header.calculate_checksum());
    TRY(m_stream->write_until_depleted(ReadonlyBytes { &header, sizeof(header) }));
    constexpr Array<u8, block_size> padding { 0 };
    TRY(m_stream->write_until_depleted(ReadonlyBytes { &padding, block_size - sizeof(header) }));
    size_t n_written = 0;
    while (n_written < bytes.size()) {
        n_written += MUST(m_stream->write_some(bytes.slice(n_written, min(bytes.size() - n_written, block_size))));
    }
    TRY(m_stream->write_until_depleted(ReadonlyBytes { &padding, block_size - (n_written % block_size) }));
    return {};
}

ErrorOr<void> TarOutputStream::add_link(StringView path, mode_t mode, StringView link_name)
{
    VERIFY(!m_finished);
    TarFileHeader header {};
    TRY(header.set_size(0));
    header.set_filename_and_prefix(path);
    header.set_type_flag(TarFileType::SymLink);
    TRY(header.set_mode(mode));
    header.set_magic(gnu_magic);
    header.set_version(gnu_version);
    header.set_link_name(link_name);
    TRY(header.calculate_checksum());
    TRY(m_stream->write_until_depleted(Bytes { &header, sizeof(header) }));
    u8 padding[block_size] = { 0 };
    TRY(m_stream->write_until_depleted(Bytes { &padding, block_size - sizeof(header) }));
    return {};
}

ErrorOr<void> TarOutputStream::finish()
{
    VERIFY(!m_finished);
    constexpr Array<u8, block_size> padding { 0 };
    // 2 empty records that are used to signify the end of the archive.
    TRY(m_stream->write_until_depleted(ReadonlyBytes { &padding, block_size }));
    TRY(m_stream->write_until_depleted(ReadonlyBytes { &padding, block_size }));
    m_finished = true;
    return {};
}

}
