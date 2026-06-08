/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// clang-format off
#include <vulkan/vulkan.h>
// clang-format on

#include <AK/Math.h>
#include <AK/Optional.h>
#include <LibGfx/SharedImageBuffer.h>
#include <UI/Qt/WebContentView.h>

#include <QByteArrayList>
#include <QCursor>
#include <QVersionNumber>
#include <QVulkanInstance>
#include <QVulkanWindow>
#include <QWidget>
#include <libdrm/drm_fourcc.h>
#include <sys/stat.h>
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

#include <WebContentViewLinuxFragShader.h>
#include <WebContentViewLinuxVertShader.h>

static Optional<u32> find_memory_type_index(VkPhysicalDevice physical_device, u32 memory_type_bits)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if (memory_type_bits & (1u << i))
            return i;
    }

    return {};
}

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
        struct stat statbuf {};
        if (fstat(dmabuf.file.fd(), &statbuf) < 0)
            return {};

        return DmaBufIdentity {
            .device = static_cast<u64>(statbuf.st_dev),
            .inode = static_cast<u64>(statbuf.st_ino),
            .size = dmabuf.size,
            .drm_format = dmabuf.drm_format,
            .pitch = dmabuf.pitch,
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

    OwnPtr<ImportedTexture> import_texture(Gfx::LinuxDmaBufHandle const& dmabuf, DmaBufIdentity const& dmabuf_identity)
    {
        auto vk_format = vk_format_from_drm_format(dmabuf.drm_format);
        if (vk_format == VK_FORMAT_UNDEFINED || dmabuf.modifier != DRM_FORMAT_MOD_LINEAR)
            return {};

        VkImageDrmFormatModifierListCreateInfoEXT modifier_list {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .pNext = nullptr,
            .drmFormatModifierCount = 1,
            .pDrmFormatModifiers = &dmabuf.modifier,
        };
        VkExternalMemoryImageCreateInfo external_memory_image_info {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &modifier_list,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo image_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external_memory_image_info,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = vk_format,
            .extent = {
                .width = static_cast<u32>(dmabuf.size.width()),
                .height = static_cast<u32>(dmabuf.size.height()),
                .depth = 1,
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VkImage image { VK_NULL_HANDLE };
        if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS)
            return {};

        VkImageSubresource subresource { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
        VkSubresourceLayout subresource_layout {};
        vkGetImageSubresourceLayout(device, image, &subresource, &subresource_layout);
        if (subresource_layout.rowPitch != dmabuf.pitch) {
            vkDestroyImage(device, image, nullptr);
            return {};
        }

        VkMemoryRequirements memory_requirements;
        vkGetImageMemoryRequirements(device, image, &memory_requirements);

        VkMemoryFdPropertiesKHR memory_fd_properties {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
            .pNext = nullptr,
            .memoryTypeBits = 0,
        };
        if (get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf.file.fd(), &memory_fd_properties) != VK_SUCCESS) {
            vkDestroyImage(device, image, nullptr);
            return {};
        }

        auto memory_type_index = find_memory_type_index(physical_device, memory_requirements.memoryTypeBits & memory_fd_properties.memoryTypeBits);
        if (!memory_type_index.has_value()) {
            vkDestroyImage(device, image, nullptr);
            return {};
        }

        int imported_fd = dup(dmabuf.file.fd());
        if (imported_fd < 0) {
            vkDestroyImage(device, image, nullptr);
            return {};
        }

        VkMemoryDedicatedAllocateInfo dedicated_allocate_info {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = nullptr,
            .image = image,
            .buffer = VK_NULL_HANDLE,
        };
        VkImportMemoryFdInfoKHR import_memory_fd_info {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .pNext = &dedicated_allocate_info,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = imported_fd,
        };
        VkMemoryAllocateInfo memory_allocate_info {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_memory_fd_info,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = *memory_type_index,
        };

        VkDeviceMemory memory { VK_NULL_HANDLE };
        if (vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
            ::close(imported_fd);
            vkDestroyImage(device, image, nullptr);
            return {};
        }

        if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
            vkFreeMemory(device, memory, nullptr);
            vkDestroyImage(device, image, nullptr);
            return {};
        }

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

    bool render(VkCommandBuffer command_buffer, Gfx::SharedImageBuffer const& shared_image_buffer, Gfx::IntSize bitmap_size, QSize target_size)
    {
        auto* imported_texture = imported_texture_for(shared_image_buffer);
        if (!imported_texture)
            return false;

        if (command_buffer == VK_NULL_HANDLE)
            return false;

        auto content_width = min(bitmap_size.width(), target_size.width());
        auto content_height = min(bitmap_size.height(), target_size.height());
        if (content_width <= 0 || content_height <= 0)
            return false;

        PushConstants push_constants {
            { static_cast<float>(target_size.width()), static_cast<float>(target_size.height()) },
            { static_cast<float>(content_width), static_cast<float>(content_height) },
            { static_cast<float>(imported_texture->size.width()), static_cast<float>(imported_texture->size.height()) },
        };

        VkViewport viewport {
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(target_size.width()),
            .height = static_cast<float>(target_size.height()),
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
    VulkanWindowRenderer(WebContentView& view, QVulkanWindow& window)
        : m_view(view)
        , m_window(window)
    {
    }

    virtual void releaseSwapChainResources() override
    {
        m_renderer.release_pipeline();
    }

    virtual void releaseResources() override
    {
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
            && m_renderer.prepare(m_window)
            && m_renderer.can_render(*paintable->shared_image_buffer);
        if (!can_render) {
            if (m_view.m_vulkan_window_container)
                m_view.m_vulkan_window_container->hide();
            m_view.update();
            m_window.frameReady();
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

        bool rendered = m_renderer.render(command_buffer, *paintable->shared_image_buffer, paintable->bitmap_size, target_size);

        vkCmdEndRenderPass(command_buffer);
        if (!rendered) {
            if (m_view.m_vulkan_window_container)
                m_view.m_vulkan_window_container->hide();
            m_view.update();
        }
        m_window.frameReady();
    }

    WebContentView& m_view;
    QVulkanWindow& m_window;
    VulkanRenderer m_renderer;
};

struct WebContentView::VulkanWindow final : public QVulkanWindow {
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
        return QVulkanWindow::event(event);
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
    m_vulkan_window->setDeviceExtensions(vulkan_dmabuf_device_extensions());
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
    setFocusProxy(m_vulkan_window_container);
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
    if (m_vulkan_window) {
        if (!current_paintable_can_use_vulkan_window()) {
            if (m_vulkan_window_container)
                m_vulkan_window_container->hide();
            update();
            return;
        }

        if (m_vulkan_window_container && !m_vulkan_window_container->isVisible()) {
            m_vulkan_window_container->setGeometry(rect());
            m_vulkan_window_container->show();
        }
        m_vulkan_window->requestUpdate();
        return;
    }

    update();
}

void WebContentView::update_vulkan_window_geometry()
{
    if (m_vulkan_window_container)
        m_vulkan_window_container->setGeometry(rect());
}

void WebContentView::set_vulkan_window_cursor(QCursor const& cursor)
{
    if (m_vulkan_window_container)
        m_vulkan_window_container->setCursor(cursor);
    if (m_vulkan_window)
        m_vulkan_window->setCursor(cursor);
}

}
