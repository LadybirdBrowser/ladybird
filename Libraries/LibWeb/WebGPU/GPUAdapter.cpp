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
#include <LibWeb/WebGPU/GPUAdapterInfo.h>
#include <LibWeb/WebGPU/GPUDevice.h>
#include <LibWeb/WebGPU/GPUSupportedFeatures.h>
#include <LibWeb/WebGPU/GPUSupportedLimits.h>
#include <LibWeb/WebIDL/Promise.h>

#include <webgpu/webgpu_cpp.h>

namespace Web::WebGPU {

GC_DEFINE_ALLOCATOR(GPUAdapter);

struct GPUAdapter::Impl {
    wgpu::Adapter adapter { nullptr };
    State state { State::Valid };
    GC::Ref<GPU> instance;
    GC::Ref<GPUSupportedFeatures> features;
    GC::Ref<GPUSupportedLimits> limits;
    // FIXME: Hook this up to the WebContent process's debug_request infra
    GC::Ref<GPUAdapterInfo> adapter_info;
};

GPUAdapter::GPUAdapter(JS::Realm& realm, Impl impl)
    : PlatformObject(realm)
    , m_impl(make<Impl>(move(impl)))
{
}

GPUAdapter::~GPUAdapter() = default;

JS::ThrowCompletionOr<GC::Ref<GPUAdapter>> GPUAdapter::create(JS::Realm& realm, GPU& instance, wgpu::Adapter adapter)
{
    wgpu::SupportedFeatures adapter_features;
    adapter.GetFeatures(&adapter_features);

    auto supported_features = GPUSupportedFeatures::create(realm);
    auto supported_features_set = supported_features->set_entries();

    bool has_core_features_and_limits = false;
    for (size_t i = 0; i < adapter_features.featureCount; ++i) {
        // https://www.w3.org/TR/webgpu/#feature-index
        auto add_feature = [&realm, &supported_features_set](StringView feature_name) {
            supported_features_set->set_add(JS::PrimitiveString::create(realm.vm(), feature_name));
        };
        switch (adapter_features.features[i]) {
        case wgpu::FeatureName::CoreFeaturesAndLimits: {
            has_core_features_and_limits = true;
            // https://www.w3.org/TR/webgpu/#core-features-and-limits
            add_feature("core-features-and-limits"sv);
            break;
        }
        case wgpu::FeatureName::DepthClipControl: {
            // https://www.w3.org/TR/webgpu/#depth-clip-control
            add_feature("depth-clip-control"sv);
            break;
        }
        case wgpu::FeatureName::Depth32FloatStencil8: {
            // https://www.w3.org/TR/webgpu/#depth32float-stencil8
            add_feature("depth32float-stencil8"sv);
            break;
        }
        case wgpu::FeatureName::TextureCompressionBC: {
            // https://www.w3.org/TR/webgpu/#texture-compression-bc
            add_feature("texture-compression-bc"sv);
            break;
        }
        case wgpu::FeatureName::TextureCompressionBCSliced3D: {
            // https://www.w3.org/TR/webgpu/#texture-compression-bc-sliced-3d
            add_feature("texture-compression-bc-sliced-3d"sv);
            break;
        }
        case wgpu::FeatureName::TextureCompressionETC2: {
            // https://www.w3.org/TR/webgpu/#texture-compression-etc2
            add_feature("texture-compression-etc2"sv);
            break;
        }
        case wgpu::FeatureName::TextureCompressionASTC: {
            // https://www.w3.org/TR/webgpu/#texture-compression-astc
            add_feature("texture-compression-astc"sv);
            break;
        }
        case wgpu::FeatureName::TextureCompressionASTCSliced3D: {
            // https://www.w3.org/TR/webgpu/#texture-compression-astc-sliced-3d
            add_feature("texture-compression-astc-sliced-3d"sv);
            break;
        }
        case wgpu::FeatureName::TimestampQuery: {
            // https://www.w3.org/TR/webgpu/#timestamp-query
            add_feature("timestamp-query"sv);
            break;
        }
        case wgpu::FeatureName::IndirectFirstInstance: {
            // https://www.w3.org/TR/webgpu/#indirect-first-instance
            add_feature("indirect-first-instance"sv);
            break;
        }
        case wgpu::FeatureName::ShaderF16: {
            // https://www.w3.org/TR/webgpu/#shader-f16
            add_feature("shader-f16"sv);
            break;
        }
        case wgpu::FeatureName::RG11B10UfloatRenderable: {
            // "rg11b10ufloat-renderable"
            add_feature("rg11b10ufloat-renderable"sv);
            break;
        }
        case wgpu::FeatureName::BGRA8UnormStorage: {
            // https://www.w3.org/TR/webgpu/#bgra8unorm-storage
            add_feature("bgra8unorm-storage"sv);
            break;
        }
        case wgpu::FeatureName::Float32Filterable: {
            // https://www.w3.org/TR/webgpu/#float32-filterable
            add_feature("float32-filterable"sv);
            break;
        }
        case wgpu::FeatureName::Float32Blendable: {
            // https://www.w3.org/TR/webgpu/#float32-blendable
            add_feature("float32-blendable"sv);
            break;
        }
        case wgpu::FeatureName::ClipDistances: {
            // https://www.w3.org/TR/webgpu/#dom-gpufeaturename-clip-distances
            add_feature("clip-distances"sv);
            break;
        }
        case wgpu::FeatureName::DualSourceBlending: {
            // https://www.w3.org/TR/webgpu/#dom-gpufeaturename-dual-source-blending
            add_feature("dual-source-blending"sv);
            break;
        }
        case wgpu::FeatureName::Subgroups: {
            // https://www.w3.org/TR/webgpu/#subgroups
            add_feature("subgroups"sv);
            break;
        }
        case wgpu::FeatureName::TextureFormatsTier1: {
            // https://www.w3.org/TR/webgpu/#texture-formats-tier1
            add_feature("texture-formats-tier1"sv);
            break;
        }
        case wgpu::FeatureName::TextureFormatsTier2: {
            // https://www.w3.org/TR/webgpu/#texture-formats-tier2
            add_feature("texture-formats-tier2"sv);
            break;
        }
        default:
            break;
        }
    }

    if (!has_core_features_and_limits)
        return realm.vm().throw_completion<JS::InternalError>(R"(Missing feature "core-features-and-limits")"_string);

    wgpu::Limits adapter_limits;
    if (!adapter.GetLimits(&adapter_limits))
        return realm.vm().throw_completion<JS::InternalError>("Unable to retrieve GPU Adapter limits"_string);

    auto supported_limits = TRY(GPUSupportedLimits::create(realm));
    supported_limits->set_max_texture_dimension1d(adapter_limits.maxTextureDimension1D);
    supported_limits->set_max_texture_dimension2d(adapter_limits.maxTextureDimension2D);
    supported_limits->set_max_texture_dimension3d(adapter_limits.maxTextureDimension3D);
    supported_limits->set_max_texture_array_layers(adapter_limits.maxTextureArrayLayers);
    supported_limits->set_max_bind_groups(adapter_limits.maxBindGroups);
    supported_limits->set_max_bind_groups_plus_vertex_buffers(adapter_limits.maxBindGroupsPlusVertexBuffers);
    supported_limits->set_max_bindings_per_bind_group(adapter_limits.maxBindingsPerBindGroup);
    supported_limits->set_max_dynamic_uniform_buffers_per_pipeline_layout(adapter_limits.maxDynamicUniformBuffersPerPipelineLayout);
    supported_limits->set_max_dynamic_storage_buffers_per_pipeline_layout(adapter_limits.maxDynamicStorageBuffersPerPipelineLayout);
    supported_limits->set_max_sampled_textures_per_shader_stage(adapter_limits.maxSampledTexturesPerShaderStage);
    supported_limits->set_max_samplers_per_shader_stage(adapter_limits.maxSamplersPerShaderStage);
    supported_limits->set_max_storage_buffers_per_shader_stage(adapter_limits.maxStorageBuffersPerShaderStage);
    supported_limits->set_max_storage_textures_per_shader_stage(adapter_limits.maxStorageTexturesPerShaderStage);
    supported_limits->set_max_uniform_buffers_per_shader_stage(adapter_limits.maxUniformBuffersPerShaderStage);
    supported_limits->set_max_uniform_buffer_binding_size(adapter_limits.maxUniformBufferBindingSize);
    supported_limits->set_max_storage_buffer_binding_size(adapter_limits.maxStorageBufferBindingSize);
    supported_limits->set_min_uniform_buffer_offset_alignment(adapter_limits.minUniformBufferOffsetAlignment);
    supported_limits->set_min_storage_buffer_offset_alignment(adapter_limits.minStorageBufferOffsetAlignment);
    supported_limits->set_max_vertex_buffers(adapter_limits.maxVertexBuffers);
    supported_limits->set_max_buffer_size(adapter_limits.maxBufferSize);
    supported_limits->set_max_vertex_attributes(adapter_limits.maxVertexAttributes);
    supported_limits->set_max_vertex_buffer_array_stride(adapter_limits.maxVertexBufferArrayStride);
    supported_limits->set_max_inter_stage_shader_variables(adapter_limits.maxInterStageShaderVariables);
    supported_limits->set_max_color_attachments(adapter_limits.maxColorAttachments);
    supported_limits->set_max_color_attachment_bytes_per_sample(adapter_limits.maxColorAttachmentBytesPerSample);
    supported_limits->set_max_compute_workgroup_storage_size(adapter_limits.maxComputeWorkgroupStorageSize);
    supported_limits->set_max_compute_invocations_per_workgroup(adapter_limits.maxComputeInvocationsPerWorkgroup);
    supported_limits->set_max_compute_workgroup_size_x(adapter_limits.maxComputeWorkgroupSizeX);
    supported_limits->set_max_compute_workgroup_size_y(adapter_limits.maxComputeWorkgroupSizeY);
    supported_limits->set_max_compute_workgroup_size_z(adapter_limits.maxComputeWorkgroupSizeZ);
    supported_limits->set_max_compute_workgroups_per_dimension(adapter_limits.maxComputeWorkgroupsPerDimension);

    wgpu::AdapterInfo adapter_info;
    if (!adapter.GetInfo(&adapter_info))
        return realm.vm().throw_completion<JS::InternalError>("Unable to retrieve GPU Adapter info"_string);

    auto vendor = MUST(String::from_utf8(StringView { adapter_info.vendor.data, adapter_info.vendor.length }));
    auto architecture = MUST(String::from_utf8(StringView { adapter_info.architecture.data, adapter_info.architecture.length }));
    auto device = MUST(String::from_utf8(StringView { adapter_info.device.data, adapter_info.device.length }));
    auto description = MUST(String::from_utf8(StringView { adapter_info.description.data, adapter_info.description.length }));
    auto subgroup_min_size = static_cast<size_t>(adapter_info.subgroupMinSize);
    auto subgroup_max_size = static_cast<size_t>(adapter_info.subgroupMaxSize);
    return realm.create<GPUAdapter>(realm, Impl { .adapter = move(adapter), .instance = instance, .features = supported_features, .limits = supported_limits, .adapter_info = TRY(GPUAdapterInfo::create(realm, move(vendor), move(architecture), move(device), move(description), subgroup_min_size, subgroup_max_size)) });
}

void GPUAdapter::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GPUAdapter);
    Base::initialize(realm);
}

void GPUAdapter::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_impl->instance);
    visitor.visit(m_impl->features);
    visitor.visit(m_impl->limits);
    visitor.visit(m_impl->adapter_info);
}

GPUAdapter::State GPUAdapter::state() const
{
    return m_impl->state;
}

void GPUAdapter::set_state(State value)
{
    m_impl->state = value;
}

GC::Ref<GPUSupportedFeatures> GPUAdapter::features() const
{
    return m_impl->features;
}

GC::Ref<GPUSupportedLimits> GPUAdapter::limits() const
{
    return m_impl->limits;
}

GC::Ref<GPUAdapterInfo> GPUAdapter::info() const
{
    return m_impl->adapter_info;
}

// https://www.w3.org/TR/webgpu/#dom-gpuadapter-requestdevice
GC::Ref<WebIDL::Promise> GPUAdapter::request_device(Optional<GPUDeviceDescriptor> descriptor)
{
    // 1. Let contentTimeline be the current Content timeline.

    // 2. Let promise be a new promise.
    auto& realm = this->realm();
    GC::Ref promise = WebIDL::create_promise(realm);

    // 3. Let adapter be this.[[adapter]].
    auto adapter = m_impl->adapter;

    // 4. Issue the initialization steps to the Device timeline of this.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, adapter, descriptor, &realm, promise]() mutable {
        wgpu::DeviceDescriptor device_descriptor;
        String device_label;
        if (descriptor.has_value()) {
            // FIXME: Set required features

            // FIXME: Set required limits

            auto const queue_label_view = descriptor.value().default_queue.label.bytes_as_string_view();
            wgpu::QueueDescriptor queue_descriptor { .nextInChain = nullptr, .label = wgpu::StringView { queue_label_view.characters_without_null_termination(), queue_label_view.length() } };

            device_label = descriptor.value().label;
            auto const device_label_view = device_label.bytes_as_string_view();
            device_descriptor = { wgpu::DeviceDescriptor::Init { .nextInChain = nullptr, .label = wgpu::StringView { device_label_view.characters_without_null_termination(), device_label_view.length() }, .defaultQueue = queue_descriptor } };
        }

        // FIXME: https://www.w3.org/TR/webgpu/#dom-gpudevice-lost
        device_descriptor.SetDeviceLostCallback(
            wgpu::CallbackMode::AllowSpontaneous,
            [](wgpu::Device const&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                auto log_device_lost = [message](StringView reason) {
                    dbgln("Device lost because of {}: {}", reason, StringView { message.data, message.length });
                };

                switch (reason) {
                case wgpu::DeviceLostReason::Unknown:
                    log_device_lost("Unknown"sv);
                    break;
                case wgpu::DeviceLostReason::Destroyed:
                    log_device_lost("Destroyed"sv);
                    break;
                case wgpu::DeviceLostReason::CallbackCancelled:
                    log_device_lost("CallbackCancelled"sv);
                    break;
                case wgpu::DeviceLostReason::FailedCreation:
                    log_device_lost("FailedCreation"sv);
                    break;
                default:
                    VERIFY_NOT_REACHED();
                }
            });
        // FIXME: https://www.w3.org/TR/webgpu/#eventdef-gpudevice-uncapturederror
        device_descriptor.SetUncapturedErrorCallback(
            [](wgpu::Device const&, wgpu::ErrorType type, wgpu::StringView message) {
                auto log_uncaptured_error = [message](StringView error_type) {
                    dbgln("{} error: {}", error_type, StringView { message.data, message.length });
                };

                switch (type) {
                case wgpu::ErrorType::Validation:
                    log_uncaptured_error("Validation"sv);
                    break;
                case wgpu::ErrorType::OutOfMemory:
                    log_uncaptured_error("Out of memory"sv);
                    break;
                case wgpu::ErrorType::Internal:
                    log_uncaptured_error("Internal"sv);
                    break;
                case wgpu::ErrorType::Unknown:
                    log_uncaptured_error("Unknown"sv);
                    break;
                default:
                    VERIFY_NOT_REACHED();
                }
            });
        m_impl->instance->wgpu().WaitAny(adapter.RequestDevice(&device_descriptor, wgpu::CallbackMode::AllowProcessEvents, [this, device_label, realm = GC::Root(realm), promise = GC::Root(promise)](wgpu::RequestDeviceStatus status, wgpu::Device native_device, char const* message) {
            // Device timeline initialization steps:
            // 1. FIXME: If any of the following requirements are unmet:
            //     - The set of values in descriptor.requiredFeatures must be a subset of those in adapter.[[features]].

            // 2. All of the requirements in the following steps must be met.
            //     1. adapter.[[state]] must not be "consumed".
            if (m_impl->state == State::Consumed) {
                HTML::TemporaryExecutionContext const context { *realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(*realm, *promise, WebIDL::OperationError::create(*realm, "Adapter has already been consumed"_utf16));
                return;
            }
            //     2. FIXME: For each [key, value] in descriptor.requiredLimits for which value is not undefined:
            //         1. key must be the name of a member of supported limits.
            //         2. value must be no better than adapter.[[limits]][key].
            //         If key’s class is alignment, value must be a power of 2 less than 2^32.

            // 3. If adapter.[[state]] is "expired" or the user agent otherwise cannot fulfill the request:
            if (m_impl->state == State::Expired || status != wgpu::RequestDeviceStatus::Success) {
                dbgln("Unable to request device: {}", message);

                // 1. Let device be a new device.
                // NOTE: This is the native_device parameter

                // 2. FIXME: Lose the device(device, "unknown").

                // 3. Assert adapter.[[state]] is "expired".
                VERIFY(m_impl->state == State::Expired);
            } else {
                // Otherwise:
                //  1. Let device be a new device with the capabilities described by descriptor.
                // NOTE: This is the native_device parameter

                // 2. Expire adapter.
                m_impl->state = State::Expired;
            }

            // 4. Issue the subsequent steps on contentTimeline.
            //      Content timeline steps:
            //      1. Let gpuDevice be a new GPUDevice instance.
            //      2. Set gpuDevice.[[device]] to device.
            //      3. FIXME: Set device.[[content device]] to gpuDevice.
            //      4. Set gpuDevice.label to descriptor.label.
            GC::Ref<GPUDevice> gpu_device = MUST(GPUDevice::create(*realm, *m_impl->instance, move(native_device), device_label));
            //      5. Resolve promise with gpuDevice.
            auto& gpu_device_realm = HTML::relevant_realm(*gpu_device);
            HTML::TemporaryExecutionContext const context { gpu_device_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            WebIDL::resolve_promise(gpu_device_realm, *promise, gpu_device);
        }),
            UINT64_MAX);
    }));

    // 5. Return promise.
    return promise;
}

}
