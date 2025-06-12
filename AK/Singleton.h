/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/Noncopyable.h>
#ifdef AK_OS_WINDOWS
// Forward declare to avoid pulling Windows.h into every file in existence.
extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long);
#    ifndef sched_yield
#        define sched_yield() Sleep(0)
#    endif
#else
#    include <sched.h>
#endif

#ifndef AK_OS_SERENITY
#    include <new>
#endif

namespace AK {

template<typename T>
struct SingletonInstanceCreator {
    static T* create()
    {
        return new T();
    }
};

template<typename T, T* (*InitFunction)() = SingletonInstanceCreator<T>::create>
class Singleton {
    AK_MAKE_NONCOPYABLE(Singleton);
    AK_MAKE_NONMOVABLE(Singleton);

public:
    Singleton() = default;

    template<bool allow_create = true>
    static T* get(Atomic<T*>& obj_var)
    {
        T* obj = obj_var.load(AK::memory_order_acquire);
        if (FlatPtr(obj) <= 0x1) {
            // If this is the first time, see if we get to initialize it
            if constexpr (allow_create) {
                if (obj == nullptr && obj_var.compare_exchange_strong(obj, (T*)0x1, AK::memory_order_acq_rel)) {
                    // We're the first one
                    obj = InitFunction();
                    obj_var.store(obj, AK::memory_order_release);
                    return obj;
                }
            }
            // Someone else was faster, wait until they're done
            while (obj == (T*)0x1) {
                sched_yield();
                obj = obj_var.load(AK::memory_order_acquire);
            }
            if constexpr (allow_create) {
                // We should always return an instance if we allow creating one
                VERIFY(obj != nullptr);
            }
            VERIFY(obj != (T*)0x1);
        }
        return obj;
    }

    T* ptr() const
    {
        return get(m_obj);
    }

    T* operator->() const
    {
        return ptr();
    }

    T& operator*() const
    {
        return *ptr();
    }

    operator T*() const
    {
        return ptr();
    }

    operator T&() const
    {
        return *ptr();
    }

    bool is_initialized() const
    {
        T* obj = m_obj.load(AK::MemoryOrder::memory_order_consume);
        return FlatPtr(obj) > 0x1;
    }

    void ensure_instance()
    {
        ptr();
    }

private:
    mutable Atomic<T*> m_obj { nullptr };
};

}

#if USING_AK_GLOBALLY
using AK::Singleton;
#endif
