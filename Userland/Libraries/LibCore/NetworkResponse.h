/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/RefCounted.h>

namespace Core {

class NetworkResponse : public RefCounted<NetworkResponse> {
public:
    virtual ~NetworkResponse() = default;

    bool is_error() const { return m_error; }

protected:
    explicit NetworkResponse() = default;

    bool m_error { false };
};

}
