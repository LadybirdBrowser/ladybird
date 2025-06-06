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

class GPUAdapterInfo final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUAdapterInfo, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUAdapterInfo);

    static JS::ThrowCompletionOr<GC::Ref<GPUAdapterInfo>> create(JS::Realm&, String, String, String, String, size_t, size_t);

    String const& vendor() const { return m_vendor; }
    String const& architecture() const { return m_architecture; }
    String const& device() const { return m_device; }
    String const& description() const { return m_description; }
    size_t subgroup_min_size() const { return m_subgroup_min_size; }
    size_t subgroup_max_size() const { return m_subgroup_max_size; }

private:
    explicit GPUAdapterInfo(JS::Realm&, String, String, String, String, size_t, size_t);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    String m_vendor;
    String m_architecture;
    String m_device;
    String m_description;
    size_t m_subgroup_min_size { 0 };
    size_t m_subgroup_max_size { 0 };
};

};
