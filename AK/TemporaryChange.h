/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

namespace AK {

template<typename T>
class TemporaryChange {
public:
    TemporaryChange(T& variable, T value)
        : m_variable(variable)
        , m_old_value(move(variable))
    {
        m_variable = move(value);
    }
    ~TemporaryChange() { m_variable = move(m_old_value); }

private:
    T& m_variable;
    T m_old_value;
};

}

#if USING_AK_GLOBALLY
using AK::TemporaryChange;
#endif
