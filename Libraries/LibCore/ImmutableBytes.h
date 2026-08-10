/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Variant.h>
#include <LibCore/Export.h>
#include <LibCore/MappedFile.h>

namespace Core {

class CORE_API ImmutableBytes {
public:
    static ErrorOr<ImmutableBytes> copy(ReadonlyBytes);
    static ErrorOr<ImmutableBytes> copy_to_readonly_mapping(ReadonlyBytes);
    static ImmutableBytes adopt(ByteBuffer);
    static ImmutableBytes adopt_mapped_file(NonnullOwnPtr<MappedFile>);
    static ErrorOr<ImmutableBytes> map_from_fd_range_and_close(int fd, StringView path, off_t offset, size_t size);

    ImmutableBytes() = default;

    [[nodiscard]] bool is_empty() const { return size() == 0; }
    [[nodiscard]] bool is_valid() const { return m_impl; }
    [[nodiscard]] bool is_file_backed() const;
    [[nodiscard]] bool is_readonly_mapped() const;

    [[nodiscard]] size_t size() const { return bytes().size(); }
    [[nodiscard]] ReadonlyBytes bytes() const LIFETIME_BOUND;
    [[nodiscard]] ErrorOr<ByteBuffer> copy_to_byte_buffer() const;

private:
    class Impl final : public RefCounted<Impl> {
    public:
        struct ReadonlyMapping {
            AK_MAKE_NONCOPYABLE(ReadonlyMapping);

        public:
            ReadonlyMapping(void* data, size_t size)
                : data(data)
                , size(size)
            {
            }
            ReadonlyMapping(ReadonlyMapping&& other)
                : data(exchange(other.data, nullptr))
                , size(exchange(other.size, 0))
            {
            }
            CORE_API ~ReadonlyMapping();

            void* data { nullptr };
            size_t size { 0 };
        };

        explicit Impl(ByteBuffer);
        explicit Impl(NonnullOwnPtr<MappedFile>);
        explicit Impl(ReadonlyMapping);

        [[nodiscard]] bool is_file_backed() const;
        [[nodiscard]] bool is_readonly_mapped() const;
        [[nodiscard]] ReadonlyBytes bytes() const LIFETIME_BOUND;

    private:
        Variant<ByteBuffer, NonnullOwnPtr<MappedFile>, ReadonlyMapping> m_storage;
    };

    explicit ImmutableBytes(NonnullRefPtr<Impl>);

    RefPtr<Impl> m_impl;
};

}
