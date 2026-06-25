/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Optional.h>
#include <AK/SingleProducerCircularQueue.h>
#include <AK/StdLibExtras.h>
#include <LibCore/AnonymousBuffer.h>

namespace Core {

// A SingleProducerCircularQueue whose storage lives in shared memory, so it can be transferred over
// IPC and mmap()ed into multiple processes.
template<typename T, size_t Size = 32>
class SharedSingleProducerCircularQueue final : public AK::SingleProducerCircularQueueBase<T, Size> {
    static_assert(IsTriviallyCopyable<T>, "Shared-memory queue items must be trivially copyable");

    using Base = AK::SingleProducerCircularQueueBase<T, Size>;
    using Data = AK::SPCQData<T, Size>;

public:
    using ValueType = T;

    SharedSingleProducerCircularQueue() = default;

    SharedSingleProducerCircularQueue(SharedSingleProducerCircularQueue const& other)
        : Base()
        , m_storage(other.m_storage)
    {
        resync_base();
    }
    SharedSingleProducerCircularQueue(SharedSingleProducerCircularQueue&& other)
        : Base()
        , m_storage(move(other.m_storage))
    {
        resync_base();
        other.set_data(nullptr);
    }
    SharedSingleProducerCircularQueue& operator=(SharedSingleProducerCircularQueue const& other)
    {
        if (this != &other) {
            m_storage = other.m_storage;
            resync_base();
        }
        return *this;
    }
    SharedSingleProducerCircularQueue& operator=(SharedSingleProducerCircularQueue&& other)
    {
        if (this != &other) {
            m_storage = move(other.m_storage);
            resync_base();
            other.set_data(nullptr);
        }
        return *this;
    }

    static ErrorOr<SharedSingleProducerCircularQueue> create()
    {
        auto buffer = TRY(AnonymousBuffer::create_with_size(sizeof(Data)));
        auto* data = new (buffer.data<void>()) Data;
        return create_internal(move(buffer), data);
    }

    static ErrorOr<SharedSingleProducerCircularQueue> create(int fd)
    {
        auto buffer = TRY(AnonymousBuffer::create_from_anon_fd(fd, sizeof(Data)));
        auto* data = reinterpret_cast<Data*>(buffer.data<void>());
        return create_internal(move(buffer), data);
    }

    ALWAYS_INLINE int fd() const { return m_storage->fd(); }
    ALWAYS_INLINE bool is_valid() const { return !m_storage.is_null(); }

private:
    class RefCountedStorage
        : public AtomicRefCounted<RefCountedStorage>
        , public AnonymousBuffer {
    public:
        RefCountedStorage(AnonymousBuffer buffer, Data* data, ByteString name)
            : AnonymousBuffer(move(buffer))
            , m_data(data)
            , m_name(move(name))
        {
        }

        ~RefCountedStorage()
        {
            dbgln_if(SHARED_QUEUE_DEBUG, "destructed SharedSingleProducerCircularQueue storage {:p} named {}", m_data, m_name);
        }

        Data* m_data { nullptr };
        ByteString m_name;
    };

    static ErrorOr<SharedSingleProducerCircularQueue> create_internal(AnonymousBuffer buffer, Data* data)
    {
        if (!data)
            return Error::from_string_literal("Unexpected error when creating shared queue from raw memory");
        auto name = ByteString::formatted("SharedSingleProducerCircularQueue@{:x}", buffer.fd());
        dbgln_if(SHARED_QUEUE_DEBUG, "successfully mmapped {} at {:p}", name, data);
        auto* storage = new (nothrow) RefCountedStorage(move(buffer), data, move(name));
        return SharedSingleProducerCircularQueue { adopt_ref(*storage) };
    }

    explicit SharedSingleProducerCircularQueue(NonnullRefPtr<RefCountedStorage> storage)
        : Base(*storage->m_data)
        , m_storage(move(storage))
    {
    }

    void resync_base() { this->set_data(m_storage ? m_storage->m_data : nullptr); }

    RefPtr<RefCountedStorage> m_storage;
};

}
