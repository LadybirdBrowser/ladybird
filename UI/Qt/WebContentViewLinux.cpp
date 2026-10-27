/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// clang-format off
#include <vulkan/vulkan.h>
// clang-format on

#include <AK/AnyOf.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/VulkanContext.h>
#include <LibGfx/VulkanImage.h>
#include <LibWebView/Application.h>
#include <UI/Qt/NativeWindowContainer.h>
#include <UI/Qt/WebContentView.h>
#ifdef LADYBIRD_HAS_WAYLAND_GPU_QUERY
#    include <UI/Qt/WaylandGpu.h>
#endif

#include <QByteArrayList>
#include <QCursor>
#include <QGuiApplication>
#include <QRegion>
#include <QTimer>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QVulkanWindow>
#include <QWidget>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace Ladybird {

static QByteArrayList vulkan_dmabuf_device_extensions()
{
    return {
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier",
        "VK_KHR_image_format_list",
    };
}

static QVulkanInstance* vulkan_instance()
{
    static QVulkanInstance* instance = []() -> QVulkanInstance* {
        auto* instance = new QVulkanInstance;
        auto supported_version = instance->supportedApiVersion();
        if (supported_version >= QVersionNumber(1, 4))
            instance->setApiVersion(QVersionNumber(1, 4));
        else if (supported_version >= QVersionNumber(1, 3))
            instance->setApiVersion(QVersionNumber(1, 3));
        else if (supported_version >= QVersionNumber(1, 2))
            instance->setApiVersion(QVersionNumber(1, 2));
        else if (supported_version >= QVersionNumber(1, 1))
            instance->setApiVersion(QVersionNumber(1, 1));
        else
            instance->setApiVersion(QVersionNumber(1, 0));

        if (!instance->create()) {
            delete instance;
            return nullptr;
        }
        return instance;
    }();
    return instance;
}

struct PresentationGpu {
    int physical_device_index { 0 };
    Gfx::GpuDeviceIdentity identity;
};

static bool device_has_extension(VkPhysicalDevice physical_device, char const* extension_name)
{
    u32 extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    Vector<VkExtensionProperties> extensions;
    extensions.resize(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, extensions.data());
    return any_of(extensions, [&](auto const& extension) {
        return strcmp(extension.extensionName, extension_name) == 0;
    });
}

// The DRM device that the window system presents on. When it does not say, as on X11, LADYBIRD_RENDER_DEVICE can name
// one. A device that the window system does not present on would have to copy every frame over to it, which not
// every driver can do, so the window system's own answer always wins.
static Optional<dev_t> presentation_drm_device()
{
#ifdef LADYBIRD_HAS_WAYLAND_GPU_QUERY
    if (auto device = wayland_compositor_main_device(); device.has_value())
        return device;
#endif
    if (auto const* path = getenv("LADYBIRD_RENDER_DEVICE"); path && *path) {
        struct stat device_stat;
        if (stat(path, &device_stat) < 0 || !S_ISCHR(device_stat.st_mode)) {
            warnln("LADYBIRD_RENDER_DEVICE does not name a device: {}", path);
            return {};
        }
        return device_stat.st_rdev;
    }
    return {};
}

// The GPU that the window system presents on. Painting there too spares each frame a copy from one GPU to another, and
// a hybrid laptop its discrete GPU's power while that GPU has nothing else to do.
static Optional<PresentationGpu> const& presentation_gpu()
{
    static NeverDestroyed<Optional<PresentationGpu>> presentation_gpu = []() -> Optional<PresentationGpu> {
        auto main_device = presentation_drm_device();
        if (!main_device.has_value())
            return {};

        auto* instance = vulkan_instance();
        if (!instance || instance->apiVersion() < QVersionNumber(1, 1))
            return {};

        u32 physical_device_count = 0;
        vkEnumeratePhysicalDevices(instance->vkInstance(), &physical_device_count, nullptr);
        Vector<VkPhysicalDevice> physical_devices;
        physical_devices.resize(physical_device_count);
        vkEnumeratePhysicalDevices(instance->vkInstance(), &physical_device_count, physical_devices.data());

        for (size_t i = 0; i < physical_devices.size(); ++i) {
            auto* physical_device = physical_devices[i];
            if (!device_has_extension(physical_device, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME))
                continue;

            VkPhysicalDeviceDrmPropertiesEXT drm_properties {};
            drm_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties {};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &drm_properties;
            vkGetPhysicalDeviceProperties2(physical_device, &properties);

            // The device may be named by either of its nodes.
            auto is_primary_node = drm_properties.hasPrimary && makedev(drm_properties.primaryMajor, drm_properties.primaryMinor) == *main_device;
            auto is_render_node = drm_properties.hasRender && makedev(drm_properties.renderMajor, drm_properties.renderMinor) == *main_device;
            if (is_primary_node || is_render_node)
                return PresentationGpu { .physical_device_index = static_cast<int>(i), .identity = Gfx::gpu_device_identity_of(physical_device) };
        }
        return {};
    }();
    return presentation_gpu.get();
}

Optional<Gfx::GpuDeviceIdentity> WebContentView::presentation_gpu_device()
{
    if (auto const& gpu = presentation_gpu(); gpu.has_value())
        return gpu->identity;
    return {};
}

// A device of our own to read frames back from GPU memory with, since a view's window has no device while it is hidden.
static Gfx::VulkanContext const* readback_vulkan_context()
{
    static NeverDestroyed<Optional<Gfx::VulkanContext>> context = []() -> Optional<Gfx::VulkanContext> {
        auto context = Gfx::create_vulkan_context(WebContentView::presentation_gpu_device());
        if (context.is_error()) {
            dbgln("Unable to create a Vulkan context to read frames back with: {}", context.error());
            return {};
        }
        return context.release_value();
    }();
    return context->has_value() ? &context->value() : nullptr;
}

RefPtr<Gfx::Bitmap const> WebContentView::cpu_bitmap_to_paint(Paintable const& paintable) const
{
    if (auto bitmap = paintable.shared_image_buffer->bitmap_if_present())
        return bitmap;

    // NB: Qt repaints this widget at every step of a resize. While the page's window covers the widget, nobody sees
    //     what it paints, and reading each frame back from GPU memory for it would be slow. The strips that hover-
    //     expanded chrome punches out of the page's window are the only part of the page that shows through.
    bool page_window_covers_widget = m_vulkan_window_container && m_vulkan_window_container->isVisible()
        && m_vertical_tab_overlay_left == 0 && m_vertical_tab_overlay_right == 0 && m_bookmarks_bar_overlay_height == 0;
    if (page_window_covers_widget)
        return {};
    return cpu_bitmap_of(paintable);
}

RefPtr<Gfx::Bitmap const> WebContentView::cpu_bitmap_of(Paintable const& paintable) const
{
    if (auto bitmap = paintable.shared_image_buffer->bitmap_if_present())
        return bitmap;

    auto const* dmabuf = paintable.shared_image_buffer->linux_dmabuf_handle();
    if (!dmabuf)
        return {};
    if (m_frame_read_from_gpu.shared_image_buffer == paintable.shared_image_buffer)
        return m_frame_read_from_gpu.bitmap;

    RefPtr<Gfx::Bitmap const> bitmap;
    if (auto const* context = readback_vulkan_context()) {
        if (auto result = Gfx::read_linux_dmabuf_into_bitmap(*context, *dmabuf); result.is_error())
            dbgln("Unable to read a frame back from GPU memory: {}", result.error());
        else
            bitmap = result.release_value();
    }
    m_frame_read_from_gpu = { paintable.shared_image_buffer, bitmap };
    return bitmap;
}

#include <WebContentViewLinuxFragShader.h>
#include <WebContentViewLinuxVertShader.h>

static VkFormat vk_format_from_drm_format(u32 drm_format)
{
    switch (drm_format) {
    case DRM_FORMAT_ARGB8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

struct WebContentView::VulkanRenderer {
    struct DmaBufIdentity {
        u64 device { 0 };
        u64 inode { 0 };
        Gfx::IntSize size;
        u32 drm_format { 0 };
        size_t pitch { 0 };
        u64 modifier { 0 };

        bool operator==(DmaBufIdentity const&) const = default;
    };

    struct ImportedTexture {
        AK_ALLOC_WITH_KMALLOC;

        VkDevice device { VK_NULL_HANDLE };
        VkDescriptorPool descriptor_pool { VK_NULL_HANDLE };
        DmaBufIdentity dmabuf_identity;
        VkImage image { VK_NULL_HANDLE };
        VkDeviceMemory memory { VK_NULL_HANDLE };
        VkImageView image_view { VK_NULL_HANDLE };
        VkDescriptorSet descriptor_set { VK_NULL_HANDLE };
        Gfx::IntSize size;

        ~ImportedTexture()
        {
            if (descriptor_set != VK_NULL_HANDLE)
                vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set);
            if (image_view != VK_NULL_HANDLE)
                vkDestroyImageView(device, image_view, nullptr);
            if (image != VK_NULL_HANDLE)
                vkDestroyImage(device, image, nullptr);
            if (memory != VK_NULL_HANDLE)
                vkFreeMemory(device, memory, nullptr);
        }
    };

    struct PushConstants {
        float target_size[2];
        float content_size[2];
        float source_size[2];
    };

    ~VulkanRenderer()
    {
        release();
    }

    void release_imported_textures()
    {
        for (auto& texture : imported_textures)
            texture = nullptr;
        next_imported_texture_slot = 0;
    }

    void release_pipeline()
    {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        render_pass = VK_NULL_HANDLE;
    }

    void release()
    {
        if (device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device);

        release_imported_textures();
        release_pipeline();

        if (descriptor_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            pipeline_layout = VK_NULL_HANDLE;
        }
        if (descriptor_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
            descriptor_set_layout = VK_NULL_HANDLE;
        }
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
            sampler = VK_NULL_HANDLE;
        }

        get_memory_fd_properties = nullptr;
        physical_device = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
    }

    bool prepare(QVulkanWindow& window)
    {
        auto new_device = window.device();
        auto new_physical_device = window.physicalDevice();
        auto new_render_pass = window.defaultRenderPass();

        if (new_device == VK_NULL_HANDLE || new_physical_device == VK_NULL_HANDLE || new_render_pass == VK_NULL_HANDLE)
            return false;

        if (device != new_device || physical_device != new_physical_device) {
            release();
            device = new_device;
            physical_device = new_physical_device;
            get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
            if (!get_memory_fd_properties)
                return false;
        }

        if (!ensure_sampler() || !ensure_descriptor_set_layout() || !ensure_pipeline_layout() || !ensure_descriptor_pool())
            return false;

        if (render_pass != new_render_pass) {
            release_pipeline();
            render_pass = new_render_pass;
        }

        return ensure_pipeline();
    }

    bool ensure_sampler()
    {
        if (sampler != VK_NULL_HANDLE)
            return true;

        VkSamplerCreateInfo sampler_info {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f,
            .maxLod = 0.0f,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
        return vkCreateSampler(device, &sampler_info, nullptr, &sampler) == VK_SUCCESS;
    }

    bool ensure_descriptor_set_layout()
    {
        if (descriptor_set_layout != VK_NULL_HANDLE)
            return true;

        VkDescriptorSetLayoutBinding binding {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr,
        };
        VkDescriptorSetLayoutCreateInfo layout_info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = 1,
            .pBindings = &binding,
        };
        return vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_set_layout) == VK_SUCCESS;
    }

    bool ensure_pipeline_layout()
    {
        if (pipeline_layout != VK_NULL_HANDLE)
            return true;

        VkPushConstantRange push_constant_range {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(PushConstants),
        };
        VkPipelineLayoutCreateInfo pipeline_layout_info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = &descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constant_range,
        };
        return vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout) == VK_SUCCESS;
    }

    bool ensure_descriptor_pool()
    {
        if (descriptor_pool != VK_NULL_HANDLE)
            return true;

        VkDescriptorPoolSize pool_size {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 8,
        };
        VkDescriptorPoolCreateInfo pool_info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = 8,
            .poolSizeCount = 1,
            .pPoolSizes = &pool_size,
        };
        return vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool) == VK_SUCCESS;
    }

    VkShaderModule create_shader_module(u32 const* code, size_t code_size)
    {
        VkShaderModuleCreateInfo shader_module_info {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = code_size,
            .pCode = code,
        };

        VkShaderModule shader_module { VK_NULL_HANDLE };
        if (vkCreateShaderModule(device, &shader_module_info, nullptr, &shader_module) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return shader_module;
    }

    bool ensure_pipeline()
    {
        if (pipeline != VK_NULL_HANDLE)
            return true;

        auto vertex_shader = create_shader_module(webcontent_vert_shader_spv, sizeof(webcontent_vert_shader_spv));
        auto fragment_shader = create_shader_module(webcontent_frag_shader_spv, sizeof(webcontent_frag_shader_spv));
        if (vertex_shader == VK_NULL_HANDLE || fragment_shader == VK_NULL_HANDLE) {
            if (vertex_shader != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, vertex_shader, nullptr);
            if (fragment_shader != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, fragment_shader, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo shader_stages[] {
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertex_shader,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragment_shader,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
        };

        VkPipelineVertexInputStateCreateInfo vertex_input {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = 0,
            .pVertexBindingDescriptions = nullptr,
            .vertexAttributeDescriptionCount = 0,
            .pVertexAttributeDescriptions = nullptr,
        };
        VkPipelineInputAssemblyStateCreateInfo input_assembly {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = VK_FALSE,
        };
        VkPipelineViewportStateCreateInfo viewport_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr,
        };
        VkPipelineRasterizationStateCreateInfo rasterization_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp = 0.0f,
            .depthBiasSlopeFactor = 0.0f,
            .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo multisample_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 1.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
        };
        VkPipelineColorBlendAttachmentState color_blend_attachment {
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo color_blend_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &color_blend_attachment,
            .blendConstants = { 0.0f, 0.0f, 0.0f, 0.0f },
        };
        VkDynamicState dynamic_states[] {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamic_states,
        };
        VkPipelineDepthStencilStateCreateInfo depth_stencil_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthTestEnable = VK_FALSE,
            .depthWriteEnable = VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f,
        };
        VkGraphicsPipelineCreateInfo pipeline_info {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stageCount = 2,
            .pStages = shader_stages,
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pTessellationState = nullptr,
            .pViewportState = &viewport_state,
            .pRasterizationState = &rasterization_state,
            .pMultisampleState = &multisample_state,
            .pDepthStencilState = &depth_stencil_state,
            .pColorBlendState = &color_blend_state,
            .pDynamicState = &dynamic_state,
            .layout = pipeline_layout,
            .renderPass = render_pass,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };

        auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        vkDestroyShaderModule(device, vertex_shader, nullptr);
        vkDestroyShaderModule(device, fragment_shader, nullptr);
        return result == VK_SUCCESS;
    }

    static Optional<DmaBufIdentity> dmabuf_identity_for(Gfx::LinuxDmaBufHandle const& dmabuf)
    {
        if (dmabuf.objects.size() != 1 || dmabuf.planes.size() != 1 || dmabuf.planes.first().offset != 0)
            return {};

        struct stat statbuf {};
        if (fstat(dmabuf.objects.first().fd(), &statbuf) < 0)
            return {};

        return DmaBufIdentity {
            .device = static_cast<u64>(statbuf.st_dev),
            .inode = static_cast<u64>(statbuf.st_ino),
            .size = dmabuf.size,
            .drm_format = dmabuf.drm_format,
            .pitch = dmabuf.planes.first().pitch,
            .modifier = dmabuf.modifier,
        };
    }

    ImportedTexture* imported_texture_for(Gfx::SharedImageBuffer const& shared_image_buffer)
    {
        auto const* dmabuf = shared_image_buffer.linux_dmabuf_handle();
        if (!dmabuf)
            return nullptr;

        auto dmabuf_identity = dmabuf_identity_for(*dmabuf);
        if (!dmabuf_identity.has_value())
            return nullptr;

        for (auto& imported_texture : imported_textures) {
            if (imported_texture && imported_texture->dmabuf_identity == *dmabuf_identity)
                return imported_texture.ptr();
        }

        auto imported_texture = import_texture(*dmabuf, *dmabuf_identity);
        if (!imported_texture)
            return nullptr;

        auto slot = next_imported_texture_slot;
        imported_textures[slot] = AK::move(imported_texture);
        next_imported_texture_slot = (next_imported_texture_slot + 1) % imported_texture_slot_count;
        return imported_textures[slot].ptr();
    }

    bool can_render(Gfx::SharedImageBuffer const& shared_image_buffer)
    {
        return imported_texture_for(shared_image_buffer);
    }

    // Whether the device can never import frames like this one, as opposed to failing to now.
    bool cannot_import_frames_like(Gfx::SharedImageBuffer const& shared_image_buffer) const
    {
        auto const* dmabuf = shared_image_buffer.linux_dmabuf_handle();
        if (!dmabuf)
            return false;
        auto vk_format = vk_format_from_drm_format(dmabuf->drm_format);
        return vk_format == VK_FORMAT_UNDEFINED || !Gfx::can_import_dmabuf_image(physical_device, vk_format, dmabuf->modifier, VK_IMAGE_USAGE_SAMPLED_BIT);
    }

    OwnPtr<ImportedTexture> import_texture(Gfx::LinuxDmaBufHandle const& dmabuf, DmaBufIdentity const& dmabuf_identity)
    {
        auto vk_format = vk_format_from_drm_format(dmabuf.drm_format);
        if (vk_format == VK_FORMAT_UNDEFINED)
            return {};
        auto imported = Gfx::import_linux_dmabuf_image(physical_device, device, get_memory_fd_properties, dmabuf, vk_format, VK_IMAGE_USAGE_SAMPLED_BIT);
        if (imported.is_error())
            return {};
        auto image = imported.value().image;
        auto memory = imported.value().memory;

        VkImageViewCreateInfo image_view_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = vk_format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkImageView image_view { VK_NULL_HANDLE };
        if (vkCreateImageView(device, &image_view_info, nullptr, &image_view) != VK_SUCCESS) {
            vkFreeMemory(device, memory, nullptr);
            vkDestroyImage(device, image, nullptr);
            return {};
        }

        VkDescriptorSet descriptor_set { VK_NULL_HANDLE };
        VkDescriptorSetAllocateInfo descriptor_set_allocate_info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = nullptr,
            .descriptorPool = descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &descriptor_set_layout,
        };
        if (vkAllocateDescriptorSets(device, &descriptor_set_allocate_info, &descriptor_set) != VK_SUCCESS) {
            vkDestroyImageView(device, image_view, nullptr);
            vkFreeMemory(device, memory, nullptr);
            vkDestroyImage(device, image, nullptr);
            return {};
        }

        VkDescriptorImageInfo descriptor_image_info {
            .sampler = sampler,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkWriteDescriptorSet descriptor_write {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &descriptor_image_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
        vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);

        auto imported_texture = make<ImportedTexture>();
        imported_texture->device = device;
        imported_texture->descriptor_pool = descriptor_pool;
        imported_texture->dmabuf_identity = dmabuf_identity;
        imported_texture->image = image;
        imported_texture->memory = memory;
        imported_texture->image_view = image_view;
        imported_texture->descriptor_set = descriptor_set;
        imported_texture->size = dmabuf.size;
        return imported_texture;
    }

    bool render(VkCommandBuffer command_buffer, Gfx::SharedImageBuffer const& shared_image_buffer, Gfx::IntSize bitmap_size, QSize target_size, bool scale_to_fit)
    {
        auto* imported_texture = imported_texture_for(shared_image_buffer);
        if (!imported_texture)
            return false;

        if (command_buffer == VK_NULL_HANDLE)
            return false;

        auto content_limit = scale_to_fit ? imported_texture->size : Gfx::IntSize { target_size.width(), target_size.height() };
        auto content_width = min(bitmap_size.width(), content_limit.width());
        auto content_height = min(bitmap_size.height(), content_limit.height());
        if (content_width <= 0 || content_height <= 0)
            return false;

        // The shader maps content pixels one-to-one onto the viewport, so scaling to fit shrinks the viewport to the
        // fitted rect and makes the content its whole extent.
        Gfx::IntRect viewport_rect { 0, 0, target_size.width(), target_size.height() };
        if (scale_to_fit)
            viewport_rect = rect_fitting_frame({ content_width, content_height }, viewport_rect.size());
        auto quad_target_size = scale_to_fit ? Gfx::IntSize { content_width, content_height } : viewport_rect.size();

        PushConstants push_constants {
            { static_cast<float>(quad_target_size.width()), static_cast<float>(quad_target_size.height()) },
            { static_cast<float>(content_width), static_cast<float>(content_height) },
            { static_cast<float>(imported_texture->size.width()), static_cast<float>(imported_texture->size.height()) },
        };

        VkViewport viewport {
            .x = static_cast<float>(viewport_rect.x()),
            .y = static_cast<float>(viewport_rect.y()),
            .width = static_cast<float>(viewport_rect.width()),
            .height = static_cast<float>(viewport_rect.height()),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D scissor {
            .offset = { 0, 0 },
            .extent = { static_cast<u32>(target_size.width()), static_cast<u32>(target_size.height()) },
        };

        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &imported_texture->descriptor_set, 0, nullptr);
        vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push_constants), &push_constants);
        vkCmdDraw(command_buffer, 4, 1, 0, 0);
        return true;
    }

    VkDevice device { VK_NULL_HANDLE };
    VkPhysicalDevice physical_device { VK_NULL_HANDLE };
    VkRenderPass render_pass { VK_NULL_HANDLE };
    VkSampler sampler { VK_NULL_HANDLE };
    VkDescriptorSetLayout descriptor_set_layout { VK_NULL_HANDLE };
    VkDescriptorPool descriptor_pool { VK_NULL_HANDLE };
    VkPipelineLayout pipeline_layout { VK_NULL_HANDLE };
    VkPipeline pipeline { VK_NULL_HANDLE };
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties { nullptr };
    static constexpr size_t imported_texture_slot_count { 2 };
    OwnPtr<ImportedTexture> imported_textures[imported_texture_slot_count];
    size_t next_imported_texture_slot { 0 };
};

struct WebContentView::VulkanWindowRenderer final : public QVulkanWindowRenderer {
    AK_ALLOC_WITH_KMALLOC;

    VulkanWindowRenderer(WebContentView& view, QVulkanWindow& window)
        : m_view(view)
        , m_window(window)
    {
    }

    virtual void releaseSwapChainResources() override
    {
        m_view.m_vulkan_window_supports_alpha_blending.clear();
        m_renderer.release_pipeline();
    }

    virtual void releaseResources() override
    {
        m_view.m_vulkan_window_supports_alpha_blending.clear();
        m_renderer.release();
    }

    virtual void logicalDeviceLost() override
    {
        m_renderer.release();
    }

    virtual void physicalDeviceLost() override
    {
        m_renderer.release();
    }

    virtual void startNextFrame() override
    {
        m_view.update_vulkan_alpha_blending_support();

        auto command_buffer = m_window.currentCommandBuffer();
        auto target_size = m_window.swapChainImageSize();
        auto render_pass = m_window.defaultRenderPass();
        auto framebuffer = m_window.currentFramebuffer();

        if (command_buffer == VK_NULL_HANDLE || render_pass == VK_NULL_HANDLE || framebuffer == VK_NULL_HANDLE || target_size.isEmpty()) {
            m_window.frameReady();
            return;
        }

        auto paintable = m_view.current_paintable();
        bool can_render = paintable.has_value()
            && !paintable->bitmap_size.is_empty()
            && m_renderer.prepare(m_window);
        if (can_render && !m_renderer.can_render(*paintable->shared_image_buffer)) {
            // NB: Reading every frame back from GPU memory would be slow, so a frame in a format or layout we cannot
            //     import makes the Compositor share its frames as bitmaps instead. A frame kept from backing stores
            //     that are gone may be from an old allocation, and says nothing about the frames to come.
            if (!paintable->shared_image_buffer->bitmap_if_present() && paintable->bitmap_id >= 0 && m_renderer.cannot_import_frames_like(*paintable->shared_image_buffer))
                WebView::Application::the().notify_compositor_gpu_presentation_unavailable();
            can_render = false;
        }
        if (!can_render) {
            m_window.frameReady();
            m_view.fall_back_to_bitmap_rendering();
            return;
        }

        auto background_color = m_view.page_background_color();
        VkClearColorValue clear_color {
            {
                static_cast<float>(background_color.red()) / 255.0f,
                static_cast<float>(background_color.green()) / 255.0f,
                static_cast<float>(background_color.blue()) / 255.0f,
                1.0f,
            },
        };
        VkClearDepthStencilValue clear_depth_stencil { 1.0f, 0 };
        VkClearValue clear_values[2] {};
        clear_values[0].color = clear_color;
        clear_values[1].depthStencil = clear_depth_stencil;

        VkRenderPassBeginInfo render_pass_begin_info {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = render_pass,
            .framebuffer = framebuffer,
            .renderArea = {
                .offset = { 0, 0 },
                .extent = {
                    .width = static_cast<u32>(target_size.width()),
                    .height = static_cast<u32>(target_size.height()),
                },
            },
            .clearValueCount = 2,
            .pClearValues = clear_values,
        };
        vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

        bool rendered = m_renderer.render(command_buffer, *paintable->shared_image_buffer, paintable->bitmap_size, target_size, m_view.m_scales_frames_to_fit);

        if (rendered && m_view.m_vulkan_window_supports_alpha_blending.value_or(false)) {
            // Clear the regions covered by hover-expanded browser chrome to transparent so widgets painted in the
            // backing store show through this native window. Top and side overlays can be visible together.
            auto device_pixel_ratio = m_view.device_pixel_ratio();

            auto punch_transparent_rect = [&](QRect const& rect) {
                if (rect.isEmpty())
                    return;

                VkClearAttachment clear_attachment {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .colorAttachment = 0,
                    .clearValue = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } } },
                };

                VkClearRect clear_rect {
                    .rect = {
                        .offset = { rect.x(), rect.y() },
                        .extent = { static_cast<u32>(rect.width()), static_cast<u32>(rect.height()) },
                    },
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                };

                vkCmdClearAttachments(command_buffer, 1, &clear_attachment, 1, &clear_rect);
            };

            auto left_width = clamp(qRound(m_view.m_vertical_tab_overlay_left * device_pixel_ratio), 0, target_size.width());
            auto right_width = clamp(qRound(m_view.m_vertical_tab_overlay_right * device_pixel_ratio), 0, target_size.width());
            auto top_height = clamp(qRound(m_view.m_bookmarks_bar_overlay_height * device_pixel_ratio), 0, target_size.height());
            punch_transparent_rect({ 0, 0, left_width, target_size.height() });
            punch_transparent_rect({ target_size.width() - right_width, 0, right_width, target_size.height() });
            punch_transparent_rect({ 0, 0, target_size.width(), top_height });
        }

        vkCmdEndRenderPass(command_buffer);
        m_window.frameReady();
        if (!rendered)
            m_view.fall_back_to_bitmap_rendering();
    }

    WebContentView& m_view;
    QVulkanWindow& m_window;
    VulkanRenderer m_renderer;
};

namespace {

class ExposeWatcher final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    ExposeWatcher(QObject* parent, Function<void()> on_exposed)
        : QObject(parent)
        , m_on_exposed(move(on_exposed))
    {
    }

private:
    virtual bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Expose && static_cast<QWindow*>(watched)->isExposed()) {
            watched->removeEventFilter(this);
            m_on_exposed();
        }
        return false;
    }

    Function<void()> m_on_exposed;
};

}

struct WebContentView::VulkanWindow final : public QVulkanWindow {
    AK_ALLOC_WITH_KMALLOC;

    explicit VulkanWindow(WebContentView& view)
        : m_view(&view)
    {
    }

    void clear_view()
    {
        m_view = nullptr;
    }

    virtual QVulkanWindowRenderer* createRenderer() override
    {
        if (!m_view)
            return nullptr;
        return new VulkanWindowRenderer(*m_view, *this);
    }

    virtual bool event(QEvent* event) override
    {
        if (m_view && m_view->handle_vulkan_window_event(event))
            return true;
        auto handled = QVulkanWindow::event(event);
        if (m_view && event->type() == QEvent::Expose)
            m_view->vulkan_window_exposure_changed();
        return handled;
    }

    WebContentView* m_view { nullptr };
};

void WebContentView::create_vulkan_window()
{
    auto* instance = vulkan_instance();
    if (!instance)
        return;

    m_vulkan_window = new VulkanWindow(*this);
    m_vulkan_window->setVulkanInstance(instance);
    if (auto const& gpu = presentation_gpu(); gpu.has_value())
        m_vulkan_window->setPhysicalDeviceIndex(gpu->physical_device_index);
    m_vulkan_window->setDeviceExtensions(vulkan_dmabuf_device_extensions());

    // Request an alpha channel so QVulkanWindow builds a blending swapchain (pre/post-multiplied composite alpha). This
    // lets us punch transparent holes for hover-expanded vertical tabs, which this native window would otherwise occlude.
    auto surface_format = m_vulkan_window->requestedFormat();
    surface_format.setAlphaBufferSize(8);
    m_vulkan_window->setFormat(surface_format);

    m_vulkan_window->setPreferredColorFormats({
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
    });

    m_vulkan_window_container = QWidget::createWindowContainer(m_vulkan_window, this);
    m_vulkan_window_container->setAcceptDrops(true);
    m_vulkan_window_container->setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    m_vulkan_window_container->setMouseTracking(true);
    m_vulkan_window_container->setGeometry(rect());
    m_vulkan_window_container->hide();

    install_native_window_container_event_forwarding(*this, *m_vulkan_window, *m_vulkan_window_container);

    m_unexposed_vulkan_window_timer.setSingleShot(true);
    m_unexposed_vulkan_window_timer.setInterval(500);
    QObject::connect(&m_unexposed_vulkan_window_timer, &QTimer::timeout, this, [this] { remap_unexposed_vulkan_window(); });
}

bool WebContentView::vulkan_window_has_native_focus() const
{
    return m_vulkan_window && QGuiApplication::focusWindow() == m_vulkan_window;
}

void WebContentView::set_vulkan_window_container_visible(bool visible)
{
    if (m_vulkan_window_container)
        set_native_window_container_visible(*this, *m_vulkan_window_container, visible);
}

void WebContentView::vulkan_window_exposure_changed()
{
    if (!m_vulkan_window)
        return;
    if (m_vulkan_window->isExposed()) {
        m_remapped_unexposed_vulkan_window = false;
        m_unexposed_vulkan_window_timer.stop();
        return;
    }
    if (!m_remapped_unexposed_vulkan_window)
        m_unexposed_vulkan_window_timer.start();
}

void WebContentView::remap_unexposed_vulkan_window()
{
    // NB: Qt stops presenting a window on Wayland when the compositor is late with a frame callback, and only presents
    //     again once one arrives. But a window that does not present is never sent one, which can leave the page
    //     frozen after a resize. Mapping the window again gets it going, once the window it is in is shown again.
    if (!m_vulkan_window || m_vulkan_window->isExposed() || !m_vulkan_window_container || !m_vulkan_window_container->isVisible())
        return;
    if (m_remapped_unexposed_vulkan_window)
        return;
    auto* top_level_window = window()->windowHandle();
    if (!top_level_window)
        return;
    if (!top_level_window->isExposed()) {
        // NB: A minimized window can stay unexposed for a long time, so wait for it rather than poll.
        if (!m_top_level_expose_watcher)
            m_top_level_expose_watcher = new ExposeWatcher(this, [this] { vulkan_window_exposure_changed(); });
        top_level_window->installEventFilter(m_top_level_expose_watcher);
        return;
    }
    m_remapped_unexposed_vulkan_window = true;
    set_vulkan_window_container_visible(false);
    set_vulkan_window_container_visible(true);
    m_vulkan_window->requestUpdate();
}

void WebContentView::fall_back_to_bitmap_rendering()
{
    // This may be invoked from within QVulkanWindow's startNextFrame(), i.e. while a frame is in flight. Hiding the
    // container synchronously destroys the window's swap chain and nulls out its Vulkan function pointers before Qt's
    // endFrame() finishes presenting the current frame, which crashes inside Qt. Defer the hide to the next event loop
    // iteration so the in-flight frame first completes on a valid swap chain.
    QTimer::singleShot(0, this, [this]() {
        set_vulkan_window_container_visible(false);
        update();
    });
}

void WebContentView::destroy_vulkan_window()
{
    setFocusProxy(nullptr);
    if (m_vulkan_window)
        m_vulkan_window->clear_view();

    if (m_vulkan_window_container) {
        delete m_vulkan_window_container;
        m_vulkan_window_container = nullptr;
        m_vulkan_window = nullptr;
        return;
    }

    delete m_vulkan_window;
    m_vulkan_window = nullptr;
}

bool WebContentView::current_paintable_can_use_vulkan_window() const
{
    auto paintable = current_paintable();
    if (!paintable.has_value())
        return false;

    return paintable->shared_image_buffer->linux_dmabuf_handle();
}

void WebContentView::schedule_vulkan_window_update()
{
    // NB: Qt paints the page itself while something covers it. Reading every frame back from GPU memory for that would
    //     be slow, so the Compositor shares the frames in CPU memory until the page is uncovered.
    set_presents_from_cpu_memory(!m_vulkan_window || is_page_covered_by_overlay() || has_floating_surface_over_page());

    if (is_page_covered_by_overlay()) {
        QTimer::singleShot(0, this, [this] {
            if (!is_page_covered_by_overlay())
                return;
            set_vulkan_window_container_visible(false);
            update();
        });
        return;
    }

    if (m_vulkan_window) {
        // NB: The page is painted by Qt while a card floats over it, so that the card shows above it.
        if (!current_paintable_can_use_vulkan_window() || has_floating_surface_over_page()) {
            set_vulkan_window_container_visible(false);
            update();
            return;
        }

        set_vulkan_window_container_visible(true);
        m_vulkan_window->requestUpdate();
        return;
    }

    update();
}

void WebContentView::update_vulkan_window_geometry()
{
    if (m_vulkan_window_container)
        m_vulkan_window_container->setGeometry(rect());
    update_vulkan_window_mask();
}

void WebContentView::update_vulkan_window_mask()
{
    if (!m_vulkan_window || !m_vulkan_window_container)
        return;

    // On X11, the native window's bounding shape exposes the regions painted transparent for hover-expanded chrome.
    // Wayland instead uses the renderer's alpha output and empties the native window's input region separately.
    auto size = m_vulkan_window_container->size();
    QRegion window_mask { QRect { QPoint { 0, 0 }, size } };

    if (m_vulkan_window_supports_alpha_blending.value_or(false)) {
        if (m_vertical_tab_overlay_left > 0)
            window_mask -= QRect { 0, 0, m_vertical_tab_overlay_left, size.height() };
        if (m_vertical_tab_overlay_right > 0)
            window_mask -= QRect { size.width() - m_vertical_tab_overlay_right, 0, m_vertical_tab_overlay_right, size.height() };
        if (m_bookmarks_bar_overlay_height > 0)
            window_mask -= QRect { 0, 0, size.width(), m_bookmarks_bar_overlay_height };
    }

    // QWindow interprets an empty mask as clearing the mask. Use a non-empty region outside the window to produce an
    // empty effective bounding shape when the overlay strips cover the entire window.
    if (window_mask.isEmpty() && !size.isEmpty())
        window_mask += QRect { size.width(), size.height(), 1, 1 };

    m_vulkan_window->setMask(window_mask);
}

void WebContentView::update_vulkan_alpha_blending_support()
{
    if (m_vulkan_window_supports_alpha_blending.has_value())
        return;

    if (!m_vulkan_window)
        return;

    auto* physical_device = m_vulkan_window->physicalDevice();
    if (physical_device == VK_NULL_HANDLE)
        return;

    auto* surface = QVulkanInstance::surfaceForWindow(m_vulkan_window);
    if (surface == VK_NULL_HANDLE)
        return;

    VkSurfaceCapabilitiesKHR capabilities {};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities) != VK_SUCCESS)
        return;

    // Our transparent tab-column holes only reach the screen if the resolved swapchain format has alpha and the surface
    // supports per-pixel blending. QVulkanWindow picks a pre/post-multiplied mode when offered. On X11 it may instead
    // select INHERIT, where the window's ARGB visual and bounding mask provide the native compositing mechanism.
    auto color_format = m_vulkan_window->colorFormat();
    bool color_format_has_alpha = color_format == VK_FORMAT_B8G8R8A8_UNORM || color_format == VK_FORMAT_R8G8B8A8_UNORM;

    VkCompositeAlphaFlagsKHR blending_modes = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    if (QGuiApplication::platformName() == "xcb")
        blending_modes |= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    m_vulkan_window_supports_alpha_blending = color_format_has_alpha && (capabilities.supportedCompositeAlpha & blending_modes) != 0;

    // Route pointer input through QWidget when it can also display widgets beneath the Vulkan surface. QWidget's
    // implicit mouse capture then preserves move and release events while the pointer is outside the browser window.
    m_vulkan_window->setFlag(Qt::WindowTransparentForInput, *m_vulkan_window_supports_alpha_blending);

    // The window mask was computed while support was still unknown; refresh it now that any stored insets can apply.
    update_vulkan_window_mask();
}

void WebContentView::set_vulkan_window_cursor(QCursor const& cursor)
{
    if (m_vulkan_window_container)
        m_vulkan_window_container->setCursor(cursor);
    if (m_vulkan_window)
        m_vulkan_window->setCursor(cursor);
}

}
