/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
 * Copyright (c) 2021, Gunnar Beutner <gbeutner@serenityos.org>
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/BitCast.h>
#include <AK/Noncopyable.h>
#include <AK/ScopeGuard.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Swift.h>
#include <AK/TypeCasts.h>
#include <AK/Types.h>

// BlockRuntime methods for Objective-C block closure support.
extern "C" void* _Block_copy(void const*);
extern "C" void _Block_release(void const*);
extern "C" size_t Block_size(void const*);

namespace AK {

// These annotations are used to avoid capturing a variable with local storage in a lambda that outlives it
#if defined(AK_COMPILER_CLANG)
#    define ESCAPING [[clang::annotate("serenity::escaping")]]
// FIXME: When we get C++23, change this to be applied to the lambda directly instead of to the types of its captures
#    define IGNORE_USE_IN_ESCAPING_LAMBDA [[clang::annotate("serenity::ignore_use_in_escaping_lambda")]]
#else
#    define ESCAPING
#    define IGNORE_USE_IN_ESCAPING_LAMBDA
#endif

namespace Detail {

#ifdef AK_HAS_OBJC_ARC
inline constexpr bool HaveObjcArc = true;
#else
inline constexpr bool HaveObjcArc = false;
#endif

// validated in TestFunction.mm
inline constexpr size_t block_layout_size = 32;

}

template<typename>
class Function;

template<typename F>
inline constexpr bool IsFunctionPointer = (IsPointer<F> && IsFunction<RemovePointer<F>>);

// Not a function pointer, and not an lvalue reference.
template<typename F>
inline constexpr bool IsFunctionObject = (!IsFunctionPointer<F> && IsRvalueReference<F&&>);

template<typename Out, typename... In>
class Function<Out(In...)> {
    AK_MAKE_NONCOPYABLE(Function);

public:
    using FunctionType = Out(In...);
    using ReturnType = Out;

    constexpr static auto AccommodateExcessiveAlignmentRequirements = true;
    constexpr static size_t ExcessiveAlignmentThreshold = 16;

    Function() = default;
    Function(nullptr_t)
    {
    }

    ~Function()
    {
        clear(false);
    }

    [[nodiscard]] ReadonlyBytes raw_capture_range() const
    {
        if (!m_size)
            return {};
        if (auto* wrapper = callable_wrapper())
            return ReadonlyBytes { wrapper->raw_callable(), m_size };
        return {};
    }

    template<typename CallableType>
    Function(CallableType&& callable)
    requires((IsFunctionObject<CallableType> && IsCallableWithArguments<CallableType, Out, In...> && !IsSame<RemoveCVReference<CallableType>, Function>))
    {
        init_with_callable(forward<CallableType>(callable), CallableKind::FunctionObject);
    }

    template<typename FunctionType>
    Function(FunctionType f)
    requires((IsFunctionPointer<FunctionType> && IsCallableWithArguments<RemovePointer<FunctionType>, Out, In...> && !IsSame<RemoveCVReference<FunctionType>, Function>))
    {
        init_with_callable(move(f), CallableKind::FunctionPointer);
    }

    template<typename BlockType>
    Function(BlockType b)
    requires((IsBlockClosure<BlockType> && IsCallableWithArguments<BlockType, Out, In...>))
    {
        init_with_callable(move(b), CallableKind::Block);
    }

    Function(Function&& other)
    {
        move_from(move(other));
    }

    // Note: Despite this method being const, a mutable lambda _may_ modify its own captures.
    Out operator()(In... in) const
    {
        auto* wrapper = callable_wrapper();
        VERIFY(wrapper);
        ++m_call_nesting_level;
        ScopeGuard guard([this] {
            if (--m_call_nesting_level == 0 && m_deferred_clear)
                const_cast<Function*>(this)->clear(false);
        });
        return wrapper->call(forward<In>(in)...);
    }

    explicit operator bool() const { return !!callable_wrapper(); }

    template<typename CallableType>
    Function& operator=(CallableType&& callable)
    requires((IsFunctionObject<CallableType> && IsCallableWithArguments<CallableType, Out, In...>))
    {
        clear();
        init_with_callable(forward<CallableType>(callable), CallableKind::FunctionObject);
        return *this;
    }

    template<typename FunctionType>
    Function& operator=(FunctionType f)
    requires((IsFunctionPointer<FunctionType> && IsCallableWithArguments<RemovePointer<FunctionType>, Out, In...>))
    {
        clear();
        if (f)
            init_with_callable(move(f), CallableKind::FunctionPointer);
        return *this;
    }

    template<typename BlockType>
    Function& operator=(BlockType&& block)
    requires((IsBlockClosure<BlockType> && IsCallableWithArguments<BlockType, Out, In...>))
    {
        clear();
        init_with_callable(static_cast<RemoveCVReference<BlockType>>(block), CallableKind::Block);
        return *this;
    }

    Function& operator=(nullptr_t)
    {
        clear();
        return *this;
    }

    Function& operator=(Function&& other)
    {
        if (this != &other) {
            clear();
            move_from(move(other));
        }
        return *this;
    }

private:
    enum class CallableKind {
        FunctionPointer,
        FunctionObject,
        Block,
    };

    class CallableWrapperBase {
    public:
        virtual ~CallableWrapperBase() = default;
        // Note: This is not const to allow storing mutable lambdas.
        virtual Out call(In...) = 0;
        virtual void init_and_swap(u8*, size_t) = 0;
        virtual void const* raw_callable() const = 0;
    };

    template<typename CallableType>
    class CallableWrapper final : public CallableWrapperBase {
        AK_MAKE_NONMOVABLE(CallableWrapper);
        AK_MAKE_NONCOPYABLE(CallableWrapper);

    public:
        explicit CallableWrapper(CallableType&& callable)
            : m_callable(move(callable))
        {
        }

        Out call(In... in) final override
        {
            return m_callable(forward<In>(in)...);
        }

        virtual ~CallableWrapper() final override
        {
            if constexpr (IsBlockClosure<CallableType>) {
                if constexpr (Detail::HaveObjcArc)
                    m_callable = nullptr;
                else
                    _Block_release(m_callable);
            }
        }

        // NOLINTNEXTLINE(readability-non-const-parameter) False positive; destination is used in a placement new expression
        void init_and_swap(u8* destination, size_t size) final override
        {
            VERIFY(size >= sizeof(CallableWrapper));
            new (destination) CallableWrapper { move(m_callable) };
        }

        void const* raw_callable() const final override
        {
            if constexpr (IsBlockClosure<CallableType>)
                return static_cast<u8 const*>(bridge_cast<void>(m_callable)) + Detail::block_layout_size;
            else
                return &m_callable;
        }

    private:
        CallableType m_callable;
    };

    enum class FunctionKind {
        NullPointer,
        Inline,
        Outline,
        Block,
    };

    CallableWrapperBase* callable_wrapper() const
    {
        switch (m_kind) {
        case FunctionKind::NullPointer:
            return nullptr;
        case FunctionKind::Inline:
        case FunctionKind::Block:
            return bit_cast<CallableWrapperBase*>(&m_storage);
        case FunctionKind::Outline:
            return *bit_cast<CallableWrapperBase**>(&m_storage);
        default:
            VERIFY_NOT_REACHED();
        }
    }

    void clear(bool may_defer = true)
    {
        bool called_from_inside_function = m_call_nesting_level > 0;
        // NOTE: This VERIFY could fail because a Function is destroyed from within itself.
        VERIFY(may_defer || !called_from_inside_function);
        if (called_from_inside_function && may_defer) {
            m_deferred_clear = true;
            return;
        }
        m_deferred_clear = false;
        auto* wrapper = callable_wrapper();
        switch (m_kind) {
        case FunctionKind::Inline:
            VERIFY(wrapper);
            wrapper->~CallableWrapperBase();
            break;
        case FunctionKind::Outline:
            VERIFY(wrapper);
            delete wrapper;
            break;
        case FunctionKind::Block:
            VERIFY(wrapper);
            wrapper->~CallableWrapperBase();
            break;
        case FunctionKind::NullPointer:
            break;
        }
        m_kind = FunctionKind::NullPointer;
    }

    template<typename Callable>
    void init_with_callable(Callable&& callable, CallableKind callable_kind)
    {
        if constexpr (alignof(Callable) > ExcessiveAlignmentThreshold && !AccommodateExcessiveAlignmentRequirements) {
            static_assert(
                alignof(Callable) <= ExcessiveAlignmentThreshold,
                "This callable object has a very large alignment requirement, "
                "check your capture list if it is a lambda expression, "
                "and make sure your callable object is not excessively aligned.");
        }
        VERIFY(m_call_nesting_level == 0);
        using WrapperType = CallableWrapper<Callable>;
        if (callable_kind == CallableKind::FunctionObject)
            m_size = sizeof(Callable);
        else
            m_size = 0;
        if constexpr (IsBlockClosure<Callable>) {
            auto block_size = Block_size(bridge_cast<void>(callable));
            VERIFY(block_size >= Detail::block_layout_size);
            m_size = block_size - Detail::block_layout_size;
        }

        if constexpr (alignof(Callable) > inline_alignment || sizeof(WrapperType) > inline_capacity) {
            *bit_cast<CallableWrapperBase**>(&m_storage) = new WrapperType(forward<Callable>(callable));
            m_kind = FunctionKind::Outline;
        } else {
            static_assert(sizeof(WrapperType) <= inline_capacity);
            if constexpr (IsBlockClosure<Callable>) {
                if constexpr (Detail::HaveObjcArc) {
                    new (m_storage) WrapperType(forward<Callable>(callable));
                } else {
                    new (m_storage) WrapperType(reinterpret_cast<Callable>(_Block_copy(callable)));
                }
                m_kind = FunctionKind::Block;
            } else {
                new (m_storage) WrapperType(forward<Callable>(callable));
                m_kind = FunctionKind::Inline;
            }
        }
    }

    void move_from(Function&& other)
    {
        VERIFY(m_call_nesting_level == 0 && other.m_call_nesting_level == 0);
        auto* other_wrapper = other.callable_wrapper();
        m_size = other.m_size;
        switch (other.m_kind) {
        case FunctionKind::NullPointer:
            break;
        case FunctionKind::Inline:
        case FunctionKind::Block:
            other_wrapper->init_and_swap(m_storage, inline_capacity);
            m_kind = other.m_kind;
            break;
        case FunctionKind::Outline:
            *bit_cast<CallableWrapperBase**>(&m_storage) = other_wrapper;
            m_kind = FunctionKind::Outline;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
        other.m_kind = FunctionKind::NullPointer;
    }

    size_t m_size { 0 };
    FunctionKind m_kind { FunctionKind::NullPointer };
    bool m_deferred_clear { false };
    mutable Atomic<u16> m_call_nesting_level { 0 };

    static constexpr size_t inline_alignment = max(alignof(CallableWrapperBase), alignof(CallableWrapperBase*));
    // Empirically determined to fit most lambdas and functions.
    static constexpr size_t inline_capacity = 4 * sizeof(void*);

    alignas(inline_alignment) u8 m_storage[inline_capacity];
} SWIFT_UNSAFE_REFERENCE;

}

#if USING_AK_GLOBALLY
using AK::Function;
using AK::IsCallableWithArguments;
#endif
