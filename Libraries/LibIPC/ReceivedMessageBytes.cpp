/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <LibIPC/ReceivedMessageBytes.h>

namespace IPC {

class ReceivedMessageBytes::Impl final : public RefCounted<ReceivedMessageBytes::Impl> {
public:
    explicit Impl(Vector<u8> bytes)
        : m_vector(move(bytes))
        , m_bytes(m_vector)
    {
    }

    Impl(ReadonlyBytes bytes, void* foreign_owner, void (*release_foreign_owner)(void*))
        : m_bytes(bytes)
        , m_foreign_owner(foreign_owner)
        , m_release_foreign_owner(release_foreign_owner)
    {
    }

    ~Impl()
    {
        if (m_release_foreign_owner)
            m_release_foreign_owner(m_foreign_owner);
    }

    ReadonlyBytes bytes() const { return m_bytes; }

private:
    Vector<u8> m_vector;
    ReadonlyBytes m_bytes;
    void* m_foreign_owner { nullptr };
    void (*m_release_foreign_owner)(void*) { nullptr };
};

ReceivedMessageBytes::ReceivedMessageBytes() = default;
ReceivedMessageBytes::ReceivedMessageBytes(ReceivedMessageBytes const&) = default;
ReceivedMessageBytes::ReceivedMessageBytes(ReceivedMessageBytes&&) = default;
ReceivedMessageBytes::~ReceivedMessageBytes() = default;

ReceivedMessageBytes& ReceivedMessageBytes::operator=(ReceivedMessageBytes const&) = default;
ReceivedMessageBytes& ReceivedMessageBytes::operator=(ReceivedMessageBytes&&) = default;

ReceivedMessageBytes::ReceivedMessageBytes(NonnullRefPtr<Impl> impl)
    : m_impl(move(impl))
{
}

ReceivedMessageBytes ReceivedMessageBytes::from_vector(Vector<u8> bytes)
{
    if (bytes.is_empty())
        return {};
    return ReceivedMessageBytes { adopt_ref(*new Impl(move(bytes))) };
}

ReceivedMessageBytes ReceivedMessageBytes::adopt_foreign_storage(ReadonlyBytes bytes, void* owner, void (*release)(void* owner))
{
    if (bytes.is_empty()) {
        release(owner);
        return {};
    }
    return ReceivedMessageBytes { adopt_ref(*new Impl(bytes, owner, release)) };
}

ReadonlyBytes ReceivedMessageBytes::bytes() const
{
    if (!m_impl)
        return {};
    return m_impl->bytes();
}

}
