/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibCore/Promise.h>
#include <LibWebGPUNative/Forward.h>

namespace WebGPUNative {

class Device;
class Instance;

class WEBGPUNATIVE_API Adapter {
public:
    friend class Device;

    explicit Adapter(Instance const&);
    Adapter(Adapter&&) noexcept;
    Adapter& operator=(Adapter&&) noexcept;
    ~Adapter();

    ErrorOr<void> initialize();

    Device device() const;

    NonnullRefPtr<Core::Promise<Device>> request_device();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
