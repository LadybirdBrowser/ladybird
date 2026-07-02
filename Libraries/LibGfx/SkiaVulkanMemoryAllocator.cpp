/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Adapted from Skia's VulkanAMDMemoryAllocator (Copyright 2018 Google LLC,
// BSD-3-Clause), which is no longer reachable through Skia's public API.

#define AK_DONT_REPLACE_STD

#include <AK/Assertions.h>
#include <LibGfx/SkiaVulkanMemoryAllocator.h>
#include <gpu/vk/VulkanTypes.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

namespace Gfx {

class SkiaVulkanMemoryAllocator final : public skgpu::VulkanMemoryAllocator {
public:
    explicit SkiaVulkanMemoryAllocator(VmaAllocator allocator)
        : m_allocator(allocator)
    {
    }

    ~SkiaVulkanMemoryAllocator() override
    {
        vmaDestroyAllocator(m_allocator);
    }

    VkResult allocateImageMemory(VkImage image, uint32_t allocation_property_flags, skgpu::VulkanBackendMemory* backend_memory) override
    {
        VmaAllocationCreateInfo info = {};
        info.usage = VMA_MEMORY_USAGE_UNKNOWN;
        info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        if (allocation_property_flags & kDedicatedAllocation_AllocationPropertyFlag)
            info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (allocation_property_flags & kLazyAllocation_AllocationPropertyFlag)
            info.requiredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        if (allocation_property_flags & kProtected_AllocationPropertyFlag)
            info.requiredFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;

        VmaAllocation allocation;
        VkResult result = vmaAllocateMemoryForImage(m_allocator, image, &info, &allocation, nullptr);
        if (result == VK_SUCCESS)
            *backend_memory = reinterpret_cast<skgpu::VulkanBackendMemory>(allocation);
        return result;
    }

    VkResult allocateBufferMemory(VkBuffer buffer, BufferUsage usage, uint32_t allocation_property_flags, skgpu::VulkanBackendMemory* backend_memory) override
    {
        VmaAllocationCreateInfo info = {};
        info.usage = VMA_MEMORY_USAGE_UNKNOWN;

        switch (usage) {
        case BufferUsage::kGpuOnly:
            info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case BufferUsage::kCpuWritesGpuReads:
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case BufferUsage::kTransfersFromCpuToGpu:
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case BufferUsage::kTransfersFromGpuToCpu:
            info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        }

        if (allocation_property_flags & kDedicatedAllocation_AllocationPropertyFlag)
            info.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if ((allocation_property_flags & kLazyAllocation_AllocationPropertyFlag) && usage == BufferUsage::kGpuOnly)
            info.preferredFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        if (allocation_property_flags & kPersistentlyMapped_AllocationPropertyFlag) {
            VERIFY(usage != BufferUsage::kGpuOnly);
            info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        if (allocation_property_flags & kProtected_AllocationPropertyFlag)
            info.requiredFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;

        VmaAllocation allocation;
        VkResult result = vmaAllocateMemoryForBuffer(m_allocator, buffer, &info, &allocation, nullptr);
        if (result == VK_SUCCESS)
            *backend_memory = reinterpret_cast<skgpu::VulkanBackendMemory>(allocation);
        return result;
    }

    void freeMemory(skgpu::VulkanBackendMemory const& memory_handle) override
    {
        auto allocation = reinterpret_cast<VmaAllocation>(memory_handle);
        vmaFreeMemory(m_allocator, allocation);
    }

    void getAllocInfo(skgpu::VulkanBackendMemory const& memory_handle, skgpu::VulkanAlloc* alloc) const override
    {
        auto allocation = reinterpret_cast<VmaAllocation>(memory_handle);
        VmaAllocationInfo vma_info;
        vmaGetAllocationInfo(m_allocator, allocation, &vma_info);

        VkMemoryPropertyFlags memory_flags;
        vmaGetMemoryTypeProperties(m_allocator, vma_info.memoryType, &memory_flags);

        uint32_t flags = 0;
        if (memory_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            flags |= skgpu::VulkanAlloc::kMappable_Flag;
        if (!(memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            flags |= skgpu::VulkanAlloc::kNoncoherent_Flag;
        if (memory_flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
            flags |= skgpu::VulkanAlloc::kLazilyAllocated_Flag;

        alloc->fMemory = vma_info.deviceMemory;
        alloc->fOffset = vma_info.offset;
        alloc->fSize = vma_info.size;
        alloc->fFlags = flags;
        alloc->fBackendMemory = memory_handle;
    }

    VkResult mapMemory(skgpu::VulkanBackendMemory const& memory_handle, void** data) override
    {
        auto allocation = reinterpret_cast<VmaAllocation>(memory_handle);
        return vmaMapMemory(m_allocator, allocation, data);
    }

    void unmapMemory(skgpu::VulkanBackendMemory const& memory_handle) override
    {
        auto allocation = reinterpret_cast<VmaAllocation>(memory_handle);
        vmaUnmapMemory(m_allocator, allocation);
    }

    VkResult flushMemory(skgpu::VulkanBackendMemory const& memory_handle, VkDeviceSize offset, VkDeviceSize size) override
    {
        auto allocation = reinterpret_cast<VmaAllocation>(memory_handle);
        return vmaFlushAllocation(m_allocator, allocation, offset, size);
    }

    VkResult invalidateMemory(skgpu::VulkanBackendMemory const& memory_handle, VkDeviceSize offset, VkDeviceSize size) override
    {
        auto allocation = reinterpret_cast<VmaAllocation>(memory_handle);
        return vmaInvalidateAllocation(m_allocator, allocation, offset, size);
    }

    std::pair<uint64_t, uint64_t> totalAllocatedAndUsedMemory() const override
    {
        VmaTotalStatistics stats;
        vmaCalculateStatistics(m_allocator, &stats);
        return { stats.total.statistics.blockBytes, stats.total.statistics.allocationBytes };
    }

private:
    VmaAllocator m_allocator { VK_NULL_HANDLE };
};

sk_sp<skgpu::VulkanMemoryAllocator> create_skia_vulkan_memory_allocator(VulkanContext const& vulkan_context)
{
    VmaVulkanFunctions functions = {};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info = {};
    // Skia only ever accesses the memory allocator from the thread that owns the GrDirectContext,
    // so internal locking can be skipped.
    info.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT | VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    info.physicalDevice = vulkan_context.physical_device;
    info.device = vulkan_context.logical_device;
    info.instance = vulkan_context.instance;
    info.vulkanApiVersion = vulkan_context.api_version;
    // Matches the block size Skia's own allocator used: a compromise between not wasting
    // unused allocated space and not making too many small allocations.
    info.preferredLargeHeapBlockSize = 4 * 1024 * 1024;
    info.pVulkanFunctions = &functions;

    VmaAllocator allocator;
    if (vmaCreateAllocator(&info, &allocator) != VK_SUCCESS)
        return nullptr;

    return sk_sp<SkiaVulkanMemoryAllocator>(new SkiaVulkanMemoryAllocator(allocator));
}

}
