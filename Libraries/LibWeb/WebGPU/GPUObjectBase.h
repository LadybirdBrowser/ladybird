/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUAdapterInfoPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebGPU {

struct GPUObjectDescriptorBase {
    String label;
};

class GPUObjectBaseMixin {
public:
    virtual ~GPUObjectBaseMixin();

    String const& label() const { return m_label; }

private:
    String m_label;
};

};
