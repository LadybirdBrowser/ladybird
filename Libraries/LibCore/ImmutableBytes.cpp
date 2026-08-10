/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/ImmutableBytes.h>
#include <LibCore/System.h>

namespace Core {

ErrorOr<ImmutableBytes> ImmutableBytes::copy(ReadonlyBytes bytes)
{
    return adopt(TRY(ByteBuffer::copy(bytes)));
}

ErrorOr<ImmutableBytes> ImmutableBytes::copy_to_readonly_mapping(ReadonlyBytes bytes)
{
    // Empty input has no bytes to protect, so it remains backed by an empty ByteBuffer.
    if (bytes.is_empty())
        return adopt(ByteBuffer {});

    auto* mapping = TRY(Core::System::allocate_anonymous_memory(bytes.size()));
    auto release_mapping = ArmedScopeGuard([&] {
        MUST(Core::System::release_address_space(mapping, bytes.size()));
    });
    __builtin_memcpy(mapping, bytes.data(), bytes.size());
    TRY(Core::System::protect_memory_readonly(mapping, bytes.size()));
    release_mapping.disarm();
    return ImmutableBytes { adopt_ref(*new Impl(Impl::ReadonlyMapping { mapping, bytes.size() })) };
}

ImmutableBytes ImmutableBytes::adopt(ByteBuffer bytes)
{
    return ImmutableBytes { adopt_ref(*new Impl(move(bytes))) };
}

ImmutableBytes ImmutableBytes::adopt_mapped_file(NonnullOwnPtr<MappedFile> mapped_file)
{
    return ImmutableBytes { adopt_ref(*new Impl(move(mapped_file))) };
}

ErrorOr<ImmutableBytes> ImmutableBytes::map_from_fd_range_and_close(int fd, StringView path, off_t offset, size_t size)
{
    return adopt_mapped_file(TRY(MappedFile::map_from_fd_range_and_close(fd, path, offset, size)));
}

bool ImmutableBytes::is_file_backed() const
{
    return m_impl && m_impl->is_file_backed();
}

bool ImmutableBytes::is_readonly_mapped() const
{
    return m_impl && m_impl->is_readonly_mapped();
}

ReadonlyBytes ImmutableBytes::bytes() const
{
    if (!m_impl)
        return {};
    return m_impl->bytes();
}

ErrorOr<ByteBuffer> ImmutableBytes::copy_to_byte_buffer() const
{
    return ByteBuffer::copy(bytes());
}

ImmutableBytes::ImmutableBytes(NonnullRefPtr<Impl> impl)
    : m_impl(move(impl))
{
}

ImmutableBytes::Impl::Impl(ByteBuffer bytes)
    : m_storage(move(bytes))
{
}

ImmutableBytes::Impl::Impl(NonnullOwnPtr<MappedFile> mapped_file)
    : m_storage(move(mapped_file))
{
}

ImmutableBytes::Impl::Impl(ReadonlyMapping mapping)
    : m_storage(move(mapping))
{
}

ImmutableBytes::Impl::ReadonlyMapping::~ReadonlyMapping()
{
    if (data)
        MUST(Core::System::release_address_space(data, size));
}

bool ImmutableBytes::Impl::is_file_backed() const
{
    return m_storage.has<NonnullOwnPtr<MappedFile>>();
}

bool ImmutableBytes::Impl::is_readonly_mapped() const
{
    return m_storage.has<ReadonlyMapping>();
}

ReadonlyBytes ImmutableBytes::Impl::bytes() const
{
    return m_storage.visit(
        [](ByteBuffer const& bytes) -> ReadonlyBytes {
            return bytes.bytes();
        },
        [](NonnullOwnPtr<MappedFile> const& mapped_file) -> ReadonlyBytes {
            return mapped_file->bytes();
        },
        [](ReadonlyMapping const& mapping) -> ReadonlyBytes {
            return { mapping.data, mapping.size };
        });
}

}
