/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Adapter;

class WEBGPUNATIVE_API Device {
public:
    explicit Device(Adapter const&);
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    ~Device();

    ErrorOr<void> initialize();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
