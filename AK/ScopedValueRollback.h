/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace AK {

template<typename T>
class ScopedValueRollback {
public:
    ScopedValueRollback(T& variable)
        : m_variable(variable)
        , m_saved_value(variable)
    {
    }

    ~ScopedValueRollback()
    {
        m_variable = m_saved_value;
    }

    void set_override_rollback_value(T const& value)
    {
        m_saved_value = value;
    }

private:
    T& m_variable;
    T m_saved_value;
};

}

#if USING_AK_GLOBALLY
using AK::ScopedValueRollback;
#endif
