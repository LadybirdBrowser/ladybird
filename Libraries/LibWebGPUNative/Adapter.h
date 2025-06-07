/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Instance;

class WEBGPUNATIVE_API Adapter {
public:
    explicit Adapter(Instance const&);
    Adapter(Adapter&&) noexcept;
    Adapter& operator=(Adapter&&) noexcept;
    ~Adapter();

    ErrorOr<void> initialize();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
