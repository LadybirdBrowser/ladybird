/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGPU/GPU.h>
#include <LibWeb/WebGPU/GPUAdapter.h>
#include <LibWeb/WebGPU/WGSLLanguageFeatures.h>
#include <LibWeb/WebIDL/Promise.h>

#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPU);

struct GPU::Impl {
    wgpu::Instance instance { nullptr };
    Bindings::GPUTextureFormat preferred_canvas_format { Bindings::GPUTextureFormat::Bgra8unorm };
    GC::Ref<WGSLLanguageFeatures> wgsl_language_features;
};

GPU::GPU(JS::Realm& realm, Impl impl)
    : PlatformObject(realm)
    , m_impl(make<Impl>(move(impl)))
{
}

GPU::~GPU() = default;

JS::ThrowCompletionOr<GC::Ref<GPU>> GPU::create(JS::Realm& realm)
{
    Vector const required_features = {
        wgpu::InstanceFeatureName::TimedWaitAny,
    };
    wgpu::InstanceDescriptor instance_descriptor {};
    instance_descriptor.requiredFeatureCount = required_features.size();
    instance_descriptor.requiredFeatures = required_features.data();
    wgpu::Instance instance = wgpu::CreateInstance(&instance_descriptor);
    if (instance == nullptr)
        return realm.vm().throw_completion<JS::InternalError>("Unable to initialize GPU"_string);

    wgpu::SupportedWGSLLanguageFeatures supported_wgsl_language_features {};
    instance.GetWGSLLanguageFeatures(&supported_wgsl_language_features);

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
    visitor.visit(m_impl->wgsl_language_features);
}

wgpu::Instance GPU::wgpu() const
{
    return m_impl->instance;
}

// https://www.w3.org/TR/webgpu/#dom-gpu-requestadapter
GC::Ref<WebIDL::Promise> GPU::request_adapter(Optional<GPURequestAdapterOptions> options)
{
    // 1. Let contentTimeline be the current Content timeline.

    // 2. Let promise be a new promise.
    auto& realm = this->realm();
    GC::Ref promise = WebIDL::create_promise(realm);

    // 3. Issue the initialization steps on the Device timeline of this.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, options, &realm, promise]() mutable {
        wgpu::RequestAdapterOptions request_adapter_options = {};

        if (options.has_value()) {
            // https://www.w3.org/TR/webgpu/#dom-gpurequestadapteroptions-featurelevel
            if (options->feature_level == "core"sv) {
                request_adapter_options.featureLevel = wgpu::FeatureLevel::Core;
            } else if (options->feature_level == "compatibility"sv) {
                request_adapter_options.featureLevel = wgpu::FeatureLevel::Compatibility;
            }

            switch (options->power_preference) {
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

            request_adapter_options.forceFallbackAdapter = options->force_fallback_adapter;
            // FIXME: Dawn does not expose xrCompatible yet
        }
        m_impl->instance.WaitAny(m_impl->instance.RequestAdapter(options.has_value() ? &request_adapter_options : nullptr, wgpu::CallbackMode::AllowProcessEvents, [this, request_adapter_options, realm = GC::Root(realm), promise = GC::Root(promise)](wgpu::RequestAdapterStatus status, wgpu::Adapter native_adapter, char const* message) {
            GC::Ptr<GPUAdapter> adapter;

            // Device timeline initialization steps:
            //  1. All of the requirements in the following steps must be met.
            //      1. options.featureLevel must be a feature level string.
            if (status == wgpu::RequestAdapterStatus::Success) {
                // If they are met and the user agent chooses to return an adapter:
                //     1. Set adapter to an adapter chosen according to the rules in Adapter Selection (https://www.w3.org/TR/webgpu/#adapter-selection) and the criteria in options, adhering to Adapter Capability Guarantees (https://www.w3.org/TR/webgpu/#adapter-capability-guarantees). Initialize the properties of adapter according to their definitions:
                //          1. Set adapter.[[limits]] and adapter.[[features]] according to the supported capabilities of the adapter. adapter.[[features]] must contain "core-features-and-limits".
                //          2. If adapter meets the criteria of a fallback adapter set adapter.[[fallback]] to true. Otherwise, set it to false.
                //          3. FIXME: Set adapter.[[xrCompatible]] to options.xrCompatible.
                adapter = MUST(GPUAdapter::create(*realm, *this, move(native_adapter)));
            } else {
                dbgln("Unable to request adapter: {}", message);
                // Otherwise:
                //     1. Let adapter be null.
                adapter = nullptr;
            }

            // 2. Issue the subsequent steps on contentTimeline.

            // Content timeline steps:
            //     1. If adapter is not null:
            if (adapter != nullptr) {
                //     1. Resolve promise with a new GPUAdapter encapsulating adapter.
                auto& gpu_adapter_realm = HTML::relevant_realm(*adapter);
                HTML::TemporaryExecutionContext const context { gpu_adapter_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(gpu_adapter_realm, *promise, adapter);
            } else {
                // 2. Otherwise, Resolve promise with null.
                HTML::TemporaryExecutionContext const context { *realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(*realm, *promise, JS::js_null());
            }
        }),
            UINT64_MAX);
    }));

    // 4. Return promise.
    return promise;
}

Bindings::GPUTextureFormat GPU::get_preferred_canvas_format() const
{
    return m_impl->preferred_canvas_format;
}

GC::Ref<WGSLLanguageFeatures> GPU::wgsl_language_features() const
{
    return m_impl->wgsl_language_features;
}

}
