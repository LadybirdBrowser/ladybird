/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/IterationDecision.h>
#include <AK/StdLibExtras.h>

namespace AK::Concepts {

template<typename T>
concept Integral = IsIntegral<T>;

template<typename T>
concept FloatingPoint = IsFloatingPoint<T>;

template<typename T>
concept Fundamental = IsFundamental<T>;

template<typename T>
concept Arithmetic = IsArithmetic<T>;

template<typename T>
concept Signed = IsSigned<T>;

template<typename T>
concept Unsigned = IsUnsigned<T>;

template<typename T>
concept Enum = IsEnum<T>;

template<typename T, typename U>
concept SameAs = IsSame<T, U>;

template<class From, class To>
concept ConvertibleTo = IsConvertible<From, To>;

template<typename U, typename... Ts>
concept OneOf = IsOneOf<U, Ts...>;

template<typename U, typename... Ts>
concept OneOfIgnoringCV = IsOneOfIgnoringCV<U, Ts...>;

template<typename U, typename... Ts>
concept OneOfIgnoringCVReference = IsOneOfIgnoringCVReference<U, Ts...>;

template<typename T, template<typename...> typename S>
concept SpecializationOf = IsSpecializationOf<T, S>;

template<typename T, typename S>
concept DerivedFrom = IsBaseOf<S, T>;

template<typename T>
concept AnyString = IsConstructible<StringView, RemoveCVReference<T> const&>;

template<typename T, typename U>
concept HashCompatible = IsHashCompatible<Detail::Decay<T>, Detail::Decay<U>>;

// Any indexable, sized, contiguous data structure.
template<typename ArrayT, typename ContainedT, typename SizeT = size_t>
concept ArrayLike = requires(ArrayT array, SizeT index) {
    {
        array[index]
    }
    -> SameAs<RemoveReference<ContainedT>&>;

    {
        array.size()
    }
    -> SameAs<SizeT>;

    {
        array.span()
    }
    -> SameAs<Span<RemoveReference<ContainedT>>>;

    {
        array.data()
    }
    -> SameAs<RemoveReference<ContainedT>*>;
};

// Any indexable data structure.
template<typename ArrayT, typename ContainedT, typename SizeT = size_t>
concept Indexable = requires(ArrayT array, SizeT index) {
    {
        array[index]
    }
    -> OneOf<RemoveReference<ContainedT>&, RemoveReference<ContainedT>>;
};

template<typename Func, typename... Args>
concept VoidFunction = requires(Func func, Args... args) {
    {
        func(args...)
    }
    -> SameAs<void>;
};

template<typename Func, typename... Args>
concept IteratorFunction = requires(Func func, Args... args) {
    {
        func(args...)
    }
    -> SameAs<IterationDecision>;
};

template<typename T, typename EndT>
concept IteratorPairWith = requires(T it, EndT end) {
    *it;
    {
        it != end
    } -> SameAs<bool>;
    ++it;
};

template<typename T>
concept IterableContainer = requires {
    {
        declval<T>().begin()
    } -> IteratorPairWith<decltype(declval<T>().end())>;
};

template<typename T, typename ValueT>
concept IterableContainerOf = IterableContainer<T> && requires {
    {
        *declval<T>().begin()
    } -> SameAs<ValueT&>;
};

template<typename Func, typename... Args>
concept FallibleFunction = requires(Func&& func, Args&&... args) {
    func(forward<Args>(args)...).is_error();
    func(forward<Args>(args)...).release_error();
    func(forward<Args>(args)...).release_value();
};

}
namespace AK::Detail {

template<typename T, typename Out, typename... Args>
inline constexpr bool IsCallableWithArguments = requires(T t) {
    {
        t(declval<Args>()...)
    } -> Concepts::ConvertibleTo<Out>;
} || requires(T t) {
    {
        t(declval<Args>()...)
    } -> Concepts::SameAs<Out>;
};

}

namespace AK {

using Detail::IsCallableWithArguments;

}

namespace AK::Concepts {

template<typename Func, typename R, typename... Args>
concept CallableAs = Detail::IsCallableWithArguments<Func, R, Args...>;

}

#if !USING_AK_GLOBALLY
namespace AK {

#endif
using AK::Concepts::Arithmetic;
using AK::Concepts::ArrayLike;
using AK::Concepts::CallableAs;
using AK::Concepts::ConvertibleTo;
using AK::Concepts::DerivedFrom;
using AK::Concepts::Enum;
using AK::Concepts::FallibleFunction;
using AK::Concepts::FloatingPoint;
using AK::Concepts::Fundamental;
using AK::Concepts::Indexable;
using AK::Concepts::Integral;
using AK::Concepts::IterableContainer;
using AK::Concepts::IterableContainerOf;
using AK::Concepts::IteratorFunction;
using AK::Concepts::IteratorPairWith;
using AK::Concepts::OneOf;
using AK::Concepts::OneOfIgnoringCV;
using AK::Concepts::OneOfIgnoringCVReference;
using AK::Concepts::SameAs;
using AK::Concepts::Signed;
using AK::Concepts::SpecializationOf;
using AK::Concepts::Unsigned;
using AK::Concepts::VoidFunction;
#if !USING_AK_GLOBALLY

}
#endif
