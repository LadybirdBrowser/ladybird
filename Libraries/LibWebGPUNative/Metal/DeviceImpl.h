/*
* Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebGPUNative/Device.h>
#include <LibWebGPUNative/Metal/Handle.h>

namespace WebGPUNative {

struct Device::Impl {
  explicit Impl(Adapter const& gpu_adapter);
  ~Impl() = default;

  ErrorOr<void> initialize();

  id metal_device() const { return m_metal_device->get(); }
  id command_queue() const { return m_command_queue->get(); }

private:
  OwnPtr<MetalDeviceHandle> m_metal_device;
  OwnPtr<MetalCommandQueueHandle> m_command_queue;
};

}
