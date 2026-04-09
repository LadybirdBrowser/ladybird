/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibCore/Export.h>

namespace Core {

// TODO: Hide the implementation from ABI
class CORE_API AnonymousBufferImpl final : public RefCounted<AnonymousBufferImpl> {
public:
    static ErrorOr<NonnullRefPtr<AnonymousBufferImpl>> create(size_t);
    static ErrorOr<NonnullRefPtr<AnonymousBufferImpl>> create(int fd, size_t);
    ~AnonymousBufferImpl();

    int fd() const { return m_fd; }
    size_t size() const { return m_size; }
    void* data() { return m_data; }
    void const* data() const { return m_data; }

private:
    AnonymousBufferImpl(int fd, size_t, void*);

    int m_fd { -1 };
    size_t m_size { 0 };
    void* m_data { nullptr };
};

class CORE_API AnonymousBuffer {
public:
    static ErrorOr<AnonymousBuffer> create_with_size(size_t);
    static ErrorOr<AnonymousBuffer> create_from_anon_fd(int fd, size_t);

    AnonymousBuffer() = default;

    bool is_valid() const { return m_impl; }

    int fd() const { return m_impl ? m_impl->fd() : -1; }
    size_t size() const { return m_impl ? m_impl->size() : 0; }

    ReadonlyBytes bytes() const
    {
        if (!m_impl)
            return {};
        return ReadonlyBytes { m_impl->data(), m_impl->size() };
    }

    template<typename T>
    T* data()
    {
        static_assert(IsVoid<T> || IsTrivial<T>);
        if (!m_impl)
            return nullptr;
        return (T*)m_impl->data();
    }

    template<typename T>
    T const* data() const
    {
        static_assert(IsVoid<T> || IsTrivial<T>);
        if (!m_impl)
            return nullptr;
        return (T const*)m_impl->data();
    }

private:
    explicit AnonymousBuffer(NonnullRefPtr<AnonymousBufferImpl> impl)
        : m_impl(move(impl))
    {
    }

    RefPtr<AnonymousBufferImpl> m_impl;
};

}
