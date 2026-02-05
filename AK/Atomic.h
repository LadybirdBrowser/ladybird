/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Platform.h>
#include <AK/Types.h>

namespace AK {

static inline void atomic_signal_fence(MemoryOrder order) noexcept
{
    return __atomic_signal_fence(order);
}

static inline void atomic_thread_fence(MemoryOrder order) noexcept
{
    return __atomic_thread_fence(order);
}

static inline void full_memory_barrier() noexcept
{
    atomic_signal_fence(AK::MemoryOrder::memory_order_acq_rel);
    atomic_thread_fence(AK::MemoryOrder::memory_order_acq_rel);
}

template<typename T>
static inline T atomic_exchange(T volatile* var, T desired, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_exchange_n(var, desired, order);
}

template<typename T, typename V = RemoveVolatile<T>>
static inline V* atomic_exchange(T volatile** var, V* desired, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_exchange_n(var, desired, order);
}

template<typename T, typename V = RemoveVolatile<T>>
static inline V* atomic_exchange(T volatile** var, nullptr_t, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_exchange_n(const_cast<V**>(var), nullptr, order);
}

template<typename T>
[[nodiscard]] static inline bool atomic_compare_exchange_strong(T volatile* var, T& expected, T desired, MemoryOrder order = memory_order_seq_cst) noexcept
{
    if (order == memory_order_acq_rel || order == memory_order_release)
        return __atomic_compare_exchange_n(var, &expected, desired, false, memory_order_release, memory_order_acquire);
    return __atomic_compare_exchange_n(var, &expected, desired, false, order, order);
}

template<typename T, typename V = RemoveVolatile<T>>
[[nodiscard]] static inline bool atomic_compare_exchange_strong(T volatile** var, V*& expected, V* desired, MemoryOrder order = memory_order_seq_cst) noexcept
{
    if (order == memory_order_acq_rel || order == memory_order_release)
        return __atomic_compare_exchange_n(var, &expected, desired, false, memory_order_release, memory_order_acquire);
    return __atomic_compare_exchange_n(var, &expected, desired, false, order, order);
}

template<typename T, typename V = RemoveVolatile<T>>
[[nodiscard]] static inline bool atomic_compare_exchange_strong(T volatile** var, V*& expected, nullptr_t, MemoryOrder order = memory_order_seq_cst) noexcept
{
    if (order == memory_order_acq_rel || order == memory_order_release)
        return __atomic_compare_exchange_n(const_cast<V**>(var), &expected, nullptr, false, memory_order_release, memory_order_acquire);
    return __atomic_compare_exchange_n(const_cast<V**>(var), &expected, nullptr, false, order, order);
}

template<typename T>
static inline T atomic_fetch_add(T volatile* var, T val, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_fetch_add(var, val, order);
}

template<typename T>
static inline T atomic_fetch_sub(T volatile* var, T val, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_fetch_sub(var, val, order);
}

template<typename T>
static inline T atomic_fetch_and(T volatile* var, T val, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_fetch_and(var, val, order);
}

template<typename T>
static inline T atomic_fetch_or(T volatile* var, T val, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_fetch_or(var, val, order);
}

template<typename T>
static inline T atomic_fetch_xor(T volatile* var, T val, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_fetch_xor(var, val, order);
}

template<typename T>
static inline T atomic_load(T volatile* var, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_load_n(var, order);
}

template<typename T, typename V = RemoveVolatile<T>>
static inline V* atomic_load(T volatile** var, MemoryOrder order = memory_order_seq_cst) noexcept
{
    return __atomic_load_n(const_cast<V**>(var), order);
}

static inline void atomic_pause()
{
#if __has_builtin(__builtin_ia32_pause)
    __builtin_ia32_pause();
#elif __has_builtin(__builtin_arm_yield)
    __builtin_arm_yield();
#endif
}

template<typename T>
static inline void atomic_store(T volatile* var, T desired, MemoryOrder order = memory_order_seq_cst) noexcept
{
    __atomic_store_n(var, desired, order);
}

template<typename T, typename V = RemoveVolatile<T>>
static inline void atomic_store(T volatile** var, V* desired, MemoryOrder order = memory_order_seq_cst) noexcept
{
    __atomic_store_n(var, desired, order);
}

template<typename T, typename V = RemoveVolatile<T>>
static inline void atomic_store(T volatile** var, nullptr_t, MemoryOrder order = memory_order_seq_cst) noexcept
{
    __atomic_store_n(const_cast<V**>(var), nullptr, order);
}

template<typename T>
static inline bool atomic_is_lock_free(T volatile* ptr = nullptr) noexcept
{
    return __atomic_is_lock_free(sizeof(T), ptr);
}

template<typename T, MemoryOrder DefaultMemoryOrder = AK::MemoryOrder::memory_order_seq_cst>
class Atomic {
    // FIXME: This should work through concepts/requires clauses, but according to the compiler,
    //        "IsIntegral is not more specialized than IsFundamental".
    //        Additionally, Enums are not fundamental types except that they behave like them in every observable way.
    static_assert(IsFundamental<T> | IsEnum<T>, "Atomic doesn't support non-primitive types, because it relies on compiler intrinsics. If you put non-primitives into it, you'll get linker errors like \"undefined reference to __atomic_store\".");
    T m_value { 0 };

public:
    Atomic() noexcept = default;
    Atomic& operator=(Atomic const&) volatile = delete;
    Atomic& operator=(Atomic&&) volatile = delete;
    Atomic(Atomic const&) = delete;
    Atomic(Atomic&&) = delete;

    constexpr Atomic(T val) noexcept
        : m_value(val)
    {
    }

    T volatile* ptr() noexcept
    {
        return &m_value;
    }

    T exchange(T desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        // We use this hack to prevent unnecessary initialization, even if T has a default constructor.
        // NOTE: Will need to investigate if it pessimizes the generated assembly.
        alignas(T) u8 buffer[sizeof(T)];
        T* ret = reinterpret_cast<T*>(buffer);
        __atomic_exchange(&m_value, &desired, ret, order);
        return *ret;
    }

    [[nodiscard]] bool compare_exchange_strong(T& expected, T desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        if (order == memory_order_acq_rel || order == memory_order_release)
            return __atomic_compare_exchange(&m_value, &expected, &desired, false, memory_order_release, memory_order_acquire);
        return __atomic_compare_exchange(&m_value, &expected, &desired, false, order, order);
    }

    ALWAYS_INLINE operator T() const volatile noexcept
    {
        return load();
    }

    ALWAYS_INLINE T load(MemoryOrder order = DefaultMemoryOrder) const volatile noexcept
    {
        alignas(T) u8 buffer[sizeof(T)];
        T* ret = reinterpret_cast<T*>(buffer);
        __atomic_load(&m_value, ret, order);
        return *ret;
    }

    // NOLINTNEXTLINE(misc-unconventional-assign-operator) We want operator= to exchange the value, so returning an object of type Atomic& here does not make sense
    ALWAYS_INLINE T operator=(T desired) volatile noexcept
    {
        store(desired);
        return desired;
    }

    ALWAYS_INLINE void store(T desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        __atomic_store(&m_value, &desired, order);
    }

    ALWAYS_INLINE bool is_lock_free() const volatile noexcept
    {
        return __atomic_is_lock_free(sizeof(m_value), &m_value);
    }
};

template<Integral T, MemoryOrder DefaultMemoryOrder>
class Atomic<T, DefaultMemoryOrder> {
    T m_value { 0 };

public:
    Atomic() noexcept = default;
    Atomic& operator=(Atomic const&) volatile = delete;
    Atomic& operator=(Atomic&&) volatile = delete;
    Atomic(Atomic const&) = delete;
    Atomic(Atomic&&) = delete;

    constexpr Atomic(T val) noexcept
        : m_value(val)
    {
    }

    T volatile* ptr() noexcept
    {
        return &m_value;
    }

    T exchange(T desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_exchange_n(&m_value, desired, order);
    }

    [[nodiscard]] bool compare_exchange_strong(T& expected, T desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        if (order == memory_order_acq_rel || order == memory_order_release)
            return __atomic_compare_exchange_n(&m_value, &expected, desired, false, memory_order_release, memory_order_acquire);
        return __atomic_compare_exchange_n(&m_value, &expected, desired, false, order, order);
    }

    ALWAYS_INLINE T operator++() volatile noexcept
    {
        return fetch_add(1) + 1;
    }

    ALWAYS_INLINE T operator++(int) volatile noexcept
    {
        return fetch_add(1);
    }

    ALWAYS_INLINE T operator+=(T val) volatile noexcept
    {
        return fetch_add(val) + val;
    }

    ALWAYS_INLINE T fetch_add(T val, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_fetch_add(&m_value, val, order);
    }

    ALWAYS_INLINE T operator--() volatile noexcept
    {
        return fetch_sub(1) - 1;
    }

    ALWAYS_INLINE T operator--(int) volatile noexcept
    {
        return fetch_sub(1);
    }

    ALWAYS_INLINE T operator-=(T val) volatile noexcept
    {
        return fetch_sub(val) - val;
    }

    ALWAYS_INLINE T fetch_sub(T val, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        T volatile* ptr = &m_value;
        // FIXME: GCC > 12 will wrongly warn on -Wstringop-overflow here with ASAN+UBSAN
#if defined(AK_COMPILER_GCC) && defined(HAS_ADDRESS_SANITIZER)
        if (!ptr)
            __builtin_unreachable();
#endif
        return __atomic_fetch_sub(ptr, val, order);
    }

    ALWAYS_INLINE T operator&=(T val) volatile noexcept
    {
        return fetch_and(val) & val;
    }

    ALWAYS_INLINE T fetch_and(T val, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_fetch_and(&m_value, val, order);
    }

    ALWAYS_INLINE T operator|=(T val) volatile noexcept
    {
        return fetch_or(val) | val;
    }

    ALWAYS_INLINE T fetch_or(T val, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_fetch_or(&m_value, val, order);
    }

    ALWAYS_INLINE T operator^=(T val) volatile noexcept
    {
        return fetch_xor(val) ^ val;
    }

    ALWAYS_INLINE T fetch_xor(T val, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_fetch_xor(&m_value, val, order);
    }

    ALWAYS_INLINE operator T() const volatile noexcept
    {
        return load();
    }

    ALWAYS_INLINE T load(MemoryOrder order = DefaultMemoryOrder) const volatile noexcept
    {
        return __atomic_load_n(&m_value, order);
    }

    // NOLINTNEXTLINE(misc-unconventional-assign-operator) We want operator= to exchange the value, so returning an object of type Atomic& here does not make sense
    ALWAYS_INLINE T operator=(T desired) volatile noexcept
    {
        store(desired);
        return desired;
    }

    ALWAYS_INLINE void store(T desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        __atomic_store_n(&m_value, desired, order);
    }

    ALWAYS_INLINE bool is_lock_free() const volatile noexcept
    {
        return __atomic_is_lock_free(sizeof(m_value), &m_value);
    }
};

template<typename T, MemoryOrder DefaultMemoryOrder>
class Atomic<T*, DefaultMemoryOrder> {
    T* m_value { nullptr };

public:
    Atomic() noexcept = default;
    Atomic& operator=(Atomic const&) volatile = delete;
    Atomic& operator=(Atomic&&) volatile = delete;
    Atomic(Atomic const&) = delete;
    Atomic(Atomic&&) = delete;

    constexpr Atomic(T* val) noexcept
        : m_value(val)
    {
    }

    T* volatile* ptr() noexcept
    {
        return &m_value;
    }

    T* exchange(T* desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_exchange_n(&m_value, desired, order);
    }

    [[nodiscard]] bool compare_exchange_strong(T*& expected, T* desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        if (order == memory_order_acq_rel || order == memory_order_release)
            return __atomic_compare_exchange_n(&m_value, &expected, desired, false, memory_order_release, memory_order_acquire);
        return __atomic_compare_exchange_n(&m_value, &expected, desired, false, order, order);
    }

    T* operator++() volatile noexcept
    {
        return fetch_add(1) + 1;
    }

    T* operator++(int) volatile noexcept
    {
        return fetch_add(1);
    }

    T* operator+=(ptrdiff_t val) volatile noexcept
    {
        return fetch_add(val) + val;
    }

    T* fetch_add(ptrdiff_t val, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_fetch_add(&m_value, val * sizeof(*m_value), order);
    }

    T* operator--() volatile noexcept
    {
        return fetch_sub(1) - 1;
    }

    T* operator--(int) volatile noexcept
    {
        return fetch_sub(1);
    }

    T* operator-=(ptrdiff_t val) volatile noexcept
    {
        return fetch_sub(val) - val;
    }

    T* fetch_sub(ptrdiff_t val, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        return __atomic_fetch_sub(&m_value, val * sizeof(*m_value), order);
    }

    operator T*() const volatile noexcept
    {
        return load();
    }

    T* load(MemoryOrder order = DefaultMemoryOrder) const volatile noexcept
    {
        return __atomic_load_n(&m_value, order);
    }

    // NOLINTNEXTLINE(misc-unconventional-assign-operator) We want operator= to exchange the value, so returning an object of type Atomic& here does not make sense
    T* operator=(T* desired) volatile noexcept
    {
        store(desired);
        return desired;
    }

    void store(T* desired, MemoryOrder order = DefaultMemoryOrder) volatile noexcept
    {
        __atomic_store_n(&m_value, desired, order);
    }

    bool is_lock_free() const volatile noexcept
    {
        return __atomic_is_lock_free(sizeof(m_value), &m_value);
    }
};

}

#if USING_AK_GLOBALLY
using AK::Atomic;
using AK::full_memory_barrier;
#endif
