/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GPUAdapter.h"

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGPU/GPU.h>
#include <LibWeb/WebGPU/WGSLLanguageFeatures.h>
#include <LibWeb/WebIDL/Promise.h>

#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPU);

struct GPU::Impl {
    wgpu::Instance instance { nullptr };
    Bindings::GPUTextureFormat preferred_canvas_format { Bindings::GPUTextureFormat::Bgra8unorm };
    GC::Ptr<WGSLLanguageFeatures> wgsl_language_features { nullptr };
};

GPU::GPU(JS::Realm& realm, Impl impl)
    : PlatformObject(realm)
    , m_impl(make<Impl>(impl))
{
}

GPU::~GPU()
{
    dawnProcSetProcs(nullptr);
}

JS::ThrowCompletionOr<GC::Ref<GPU>> GPU::create(JS::Realm& realm)
{
    dawnProcSetProcs(&dawn::native::GetProcs());

    wgpu::InstanceDescriptor instance_descriptor {};
    wgpu::Instance instance = wgpu::CreateInstance(&instance_descriptor);
    if (instance == nullptr)
        return realm.vm().throw_completion<JS::InternalError>("Unable to initialize GPU"_string);

    wgpu::SupportedWGSLLanguageFeatures supported_wgsl_language_features {};
    if (!instance.GetWGSLLanguageFeatures(&supported_wgsl_language_features))
        return realm.vm().throw_completion<JS::InternalError>("Unable to retrieve WGSL language features"_string);

    auto wgsl_language_features = WGSLLanguageFeatures::create(realm);
    auto wgsl_language_features_set = wgsl_language_features->set_entries();
    for (size_t i = 0; i < supported_wgsl_language_features.featureCount; ++i) {
        // https://www.w3.org/TR/WGSL/#language-extensions-sec
        auto add_feature = [&realm, &wgsl_language_features_set](StringView feature_name) {
            wgsl_language_features_set->set_add(JS::PrimitiveString::create(realm.vm(), feature_name));
        };
        switch (supported_wgsl_language_features.features[i]) {
        case wgpu::WGSLLanguageFeatureName::ReadonlyAndReadwriteStorageTextures: {
            add_feature("readonly_and_readwrite_storage_textures"sv);
            break;
        }
        case wgpu::WGSLLanguageFeatureName::Packed4x8IntegerDotProduct: {
            add_feature("packed_4x8_integer_dot_product"sv);
            break;
        }
        case wgpu::WGSLLanguageFeatureName::UnrestrictedPointerParameters: {
            add_feature("unrestricted_pointer_parameters"sv);
            break;
        }
        case wgpu::WGSLLanguageFeatureName::PointerCompositeAccess: {
            add_feature("pointer_composite_access"sv);
            break;
        }
        default:
            break;
        }
    }

    return realm.create<GPU>(realm, Impl { .instance = move(instance), .wgsl_language_features = wgsl_language_features });
}

void GPU::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPU);
    Base::initialize(realm);
}

void GPU::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// https://www.w3.org/TR/webgpu/#dom-gpu-requestadapter
GC::Ref<WebIDL::Promise> GPU::request_adapter(GPURequestAdapterOptions const& options)
{
    wgpu::RequestAdapterOptions request_adapter_options = {};

    // https://www.w3.org/TR/webgpu/#dom-gpurequestadapteroptions-featurelevel
    if (options.feature_level == "core"_string) {
        request_adapter_options.featureLevel = wgpu::FeatureLevel::Core;
    } else if (options.feature_level == "compatibility") {
        request_adapter_options.featureLevel = wgpu::FeatureLevel::Compatibility;
    }

    switch (options.power_preference) {
    case Bindings::GPUPowerPreference::HighPerformance: {
        request_adapter_options.powerPreference = wgpu::PowerPreference::HighPerformance;
        break;
    }
    case Bindings::GPUPowerPreference::LowPower: {
        request_adapter_options.powerPreference = wgpu::PowerPreference::LowPower;
        break;
    }
    default:
        break;
    }

    request_adapter_options.forceFallbackAdapter = options.force_fallback_adapter;

    // FIXME: 1. Let contentTimeline be the current Content timeline.

    // 2. Let promise be a new promise.
    auto& realm = this->realm();
    GC::Ref promise = WebIDL::create_promise(realm);

    // FIXME: 3. Issue the initialization steps on the Device timeline of this.
    auto fut = m_impl->instance.RequestAdapter(&request_adapter_options, wgpu::CallbackMode::AllowProcessEvents, [&realm, promise](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, char const* message) {
        if (status == wgpu::RequestAdapterStatus::Success) {
            auto gpu_adapter = MUST(GPUAdapter::create(realm, move(adapter)));
            auto& gpu_adapter_realm = HTML::relevant_realm(gpu_adapter);
            HTML::TemporaryExecutionContext const context { gpu_adapter_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            WebIDL::resolve_promise(gpu_adapter_realm, promise, gpu_adapter);
        } else {
            HTML::TemporaryExecutionContext const context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            WebIDL::reject_promise(realm, *promise, WebIDL::InvalidStateError::create(realm, MUST(String::formatted("{}", message))));
        }
    });

    m_impl->instance.WaitAny(fut, 0);

    // 4. Return promise.
    return promise;
}

Bindings::GPUTextureFormat GPU::get_preferred_canvas_format() const
{
    return m_impl->preferred_canvas_format;
}

WGSLLanguageFeatures const* GPU::wgsl_language_features() const
{
    if (!m_impl->wgsl_language_features)
        m_impl->wgsl_language_features = WGSLLanguageFeatures::create(realm());
    return m_impl->wgsl_language_features;
}

}
