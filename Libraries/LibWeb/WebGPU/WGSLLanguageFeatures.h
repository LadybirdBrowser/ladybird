/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/WGSLLanguageFeaturesPrototype.h>

namespace Web::WebGPU {

// https://www.w3.org/TR/webgpu/#gpuwgsllanguagefeatures
class WGSLLanguageFeatures final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WGSLLanguageFeatures, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WGSLLanguageFeatures);

    static GC::Ref<WGSLLanguageFeatures> create(JS::Realm&);
    ~WGSLLanguageFeatures() override = default;

    GC::Ref<JS::Set> set_entries() const { return m_set_entries; }
    bool has_state(FlyString const&) const;

    void on_set_modified_from_js(Badge<Bindings::WGSLLanguageFeaturesPrototype>) { }

private:
    WGSLLanguageFeatures(JS::Realm&);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    GC::Ref<JS::Set> m_set_entries;
};

}
