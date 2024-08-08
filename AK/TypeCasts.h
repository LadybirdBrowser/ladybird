/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/Forward.h>
#include <AK/Platform.h>

namespace AK {

template<typename OutputType, typename InputType>
ALWAYS_INLINE bool is(InputType& input)
{
    static_assert(!SameAs<OutputType, InputType>);
    if constexpr (requires { input.template fast_is<OutputType>(); }) {
        return input.template fast_is<OutputType>();
    }
    return dynamic_cast<CopyConst<InputType, OutputType>*>(&input);
}

template<typename OutputType, typename InputType>
ALWAYS_INLINE bool is(InputType* input)
{
    return input && is<OutputType>(*input);
}

template<typename OutputType, typename InputType>
ALWAYS_INLINE bool is(NonnullRefPtr<InputType> const& input)
{
    return is<OutputType>(*input);
}

template<typename OutputType, typename InputType>
ALWAYS_INLINE CopyConst<InputType, OutputType>* verify_cast(InputType* input)
{
    static_assert(IsBaseOf<InputType, OutputType>);
    VERIFY(!input || is<OutputType>(*input));
    return static_cast<CopyConst<InputType, OutputType>*>(input);
}

template<typename OutputType, typename InputType>
ALWAYS_INLINE CopyConst<InputType, OutputType>& verify_cast(InputType& input)
{
    static_assert(IsBaseOf<InputType, OutputType>);
    VERIFY(is<OutputType>(input));
    return static_cast<CopyConst<InputType, OutputType>&>(input);
}

}

#if USING_AK_GLOBALLY
using AK::is;
using AK::verify_cast;
#endif
