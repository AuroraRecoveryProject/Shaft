#include "Public/utils.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

#if defined(__ANDROID__)
namespace {
int64_t recovery_now_us()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec / 1000);
}

bool recovery_profile_enabled()
{
    static bool enabled = std::getenv("SHAFT_RECOVERY_PROFILE") != nullptr;
    return enabled;
}

struct hw_module_t;
struct hw_device_t;

struct hw_module_methods_t {
    int (*open)(const hw_module_t *module, const char *id, hw_device_t **device);
};

struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[10];
};

struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(hw_device_t *device);
};

struct hwvulkan_device_t {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

VkResult recovery_vk_enumerate_instance_version(uint32_t *version)
{
    if (version != nullptr) {
        *version = VK_API_VERSION_1_1;
    }
    return VK_SUCCESS;
}

VkResult recovery_vk_enumerate_instance_layer_properties(uint32_t *count, VkLayerProperties *properties)
{
    (void)properties;
    if (count != nullptr) {
        *count = 0;
    }
    return VK_SUCCESS;
}

VkResult recovery_vk_enumerate_device_layer_properties(VkPhysicalDevice physicalDevice, uint32_t *count, VkLayerProperties *properties)
{
    (void)physicalDevice;
    (void)properties;
    if (count != nullptr) {
        *count = 0;
    }
    return VK_SUCCESS;
}

struct RecoveryVulkanHostImage {
    sk_sp<SkSurface> surface;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *data = nullptr;
    size_t rowBytes = 0;
    VkDeviceSize memorySize = 0;
    bool memoryCoherent = false;
};

struct RecoveryVulkanSurface {
    void *library = nullptr;
    hw_device_t *halDevice = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    skgpu::VulkanExtensions extensions;
    sk_sp<skgpu::VulkanMemoryAllocator> memoryAllocator;
    sk_sp<GrDirectContext> context;
    sk_sp<SkSurface> surface;
    bool usesHostImageSurface = false;
    int width = 0;
    int height = 0;
    RecoveryVulkanHostImage hostImage;

    PFN_vkVoidFunction getProc(const char *name, VkInstance procInstance, VkDevice procDevice) const
    {
        if (std::strcmp(name, "vkEnumerateInstanceVersion") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(recovery_vk_enumerate_instance_version);
        }
        if (std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(recovery_vk_enumerate_instance_layer_properties);
        }
        if (std::strcmp(name, "vkEnumerateDeviceLayerProperties") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(recovery_vk_enumerate_device_layer_properties);
        }
        if (std::strcmp(name, "vkCreateInstance") == 0 && createInstance != nullptr) {
            return reinterpret_cast<PFN_vkVoidFunction>(createInstance);
        }
        if (std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0 && enumerateInstanceExtensionProperties != nullptr) {
            return reinterpret_cast<PFN_vkVoidFunction>(enumerateInstanceExtensionProperties);
        }
        if (procDevice != VK_NULL_HANDLE && getDeviceProcAddr != nullptr) {
            if (auto proc = getDeviceProcAddr(procDevice, name)) {
                return proc;
            }
        }
        return getInstanceProcAddr(procInstance, name);
    }
};

struct RecoveryVulkanAllocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceSize offset = 0;
    uint32_t flags = 0;
};

template <typename T>
T vk_proc(const RecoveryVulkanSurface &state, const char *name, VkInstance instance = VK_NULL_HANDLE, VkDevice device = VK_NULL_HANDLE);

class RecoveryVulkanMemoryAllocator final : public skgpu::VulkanMemoryAllocator {
public:
    explicit RecoveryVulkanMemoryAllocator(const RecoveryVulkanSurface &state)
        : device_(state.device)
        , allocateMemory_(vk_proc<PFN_vkAllocateMemory>(state, "vkAllocateMemory", state.instance, state.device))
        , freeMemory_(vk_proc<PFN_vkFreeMemory>(state, "vkFreeMemory", state.instance, state.device))
        , mapMemory_(vk_proc<PFN_vkMapMemory>(state, "vkMapMemory", state.instance, state.device))
        , unmapMemory_(vk_proc<PFN_vkUnmapMemory>(state, "vkUnmapMemory", state.instance, state.device))
        , flushMappedMemoryRanges_(vk_proc<PFN_vkFlushMappedMemoryRanges>(state, "vkFlushMappedMemoryRanges", state.instance, state.device))
        , invalidateMappedMemoryRanges_(vk_proc<PFN_vkInvalidateMappedMemoryRanges>(state, "vkInvalidateMappedMemoryRanges", state.instance, state.device))
        , getImageMemoryRequirements_(vk_proc<PFN_vkGetImageMemoryRequirements>(state, "vkGetImageMemoryRequirements", state.instance, state.device))
        , getBufferMemoryRequirements_(vk_proc<PFN_vkGetBufferMemoryRequirements>(state, "vkGetBufferMemoryRequirements", state.instance, state.device))
    {
        auto getMemoryProperties = vk_proc<PFN_vkGetPhysicalDeviceMemoryProperties>(state, "vkGetPhysicalDeviceMemoryProperties", state.instance, VK_NULL_HANDLE);
        if (getMemoryProperties != nullptr) {
            getMemoryProperties(state.physicalDevice, &memoryProperties_);
        }
    }

    VkResult allocateImageMemory(VkImage image, uint32_t allocationPropertyFlags, skgpu::VulkanBackendMemory *memory) override
    {
        if (getImageMemoryRequirements_ == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        VkMemoryRequirements requirements;
        getImageMemoryRequirements_(device_, image, &requirements);
        VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if ((allocationPropertyFlags & kLazyAllocation_AllocationPropertyFlag) != 0) {
            wanted |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        }
        return allocate(requirements, wanted, allocationPropertyFlags, memory);
    }

    VkResult allocateBufferMemory(VkBuffer buffer, BufferUsage usage, uint32_t allocationPropertyFlags, skgpu::VulkanBackendMemory *memory) override
    {
        if (getBufferMemoryRequirements_ == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        VkMemoryRequirements requirements;
        getBufferMemoryRequirements_(device_, buffer, &requirements);
        VkMemoryPropertyFlags wanted = 0;
        switch (usage) {
        case BufferUsage::kGpuOnly:
            wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case BufferUsage::kCpuWritesGpuReads:
        case BufferUsage::kTransfersFromCpuToGpu:
            wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case BufferUsage::kTransfersFromGpuToCpu:
            wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        }
        return allocate(requirements, wanted, allocationPropertyFlags, memory);
    }

    void getAllocInfo(const skgpu::VulkanBackendMemory &memory, skgpu::VulkanAlloc *alloc) const override
    {
        auto *allocation = reinterpret_cast<RecoveryVulkanAllocation *>(memory);
        if (allocation == nullptr || alloc == nullptr) {
            return;
        }
        alloc->fMemory = allocation->memory;
        alloc->fOffset = allocation->offset;
        alloc->fSize = allocation->size;
        alloc->fFlags = allocation->flags;
        alloc->fBackendMemory = memory;
    }

    void *mapMemory(const skgpu::VulkanBackendMemory &memory) override
    {
        auto *allocation = reinterpret_cast<RecoveryVulkanAllocation *>(memory);
        if (allocation == nullptr || mapMemory_ == nullptr) {
            return nullptr;
        }
        void *data = nullptr;
        if (mapMemory_(device_, allocation->memory, allocation->offset, allocation->size, 0, &data) != VK_SUCCESS) {
            return nullptr;
        }
        return data;
    }

    void unmapMemory(const skgpu::VulkanBackendMemory &memory) override
    {
        auto *allocation = reinterpret_cast<RecoveryVulkanAllocation *>(memory);
        if (allocation != nullptr && unmapMemory_ != nullptr) {
            unmapMemory_(device_, allocation->memory);
        }
    }

    void flushMappedMemory(const skgpu::VulkanBackendMemory &memory, VkDeviceSize offset, VkDeviceSize size) override
    {
        auto *allocation = reinterpret_cast<RecoveryVulkanAllocation *>(memory);
        if (allocation == nullptr || flushMappedMemoryRanges_ == nullptr) {
            return;
        }
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = allocation->memory;
        range.offset = allocation->offset + offset;
        range.size = size;
        flushMappedMemoryRanges_(device_, 1, &range);
    }

    void invalidateMappedMemory(const skgpu::VulkanBackendMemory &memory, VkDeviceSize offset, VkDeviceSize size) override
    {
        auto *allocation = reinterpret_cast<RecoveryVulkanAllocation *>(memory);
        if (allocation == nullptr || invalidateMappedMemoryRanges_ == nullptr) {
            return;
        }
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = allocation->memory;
        range.offset = allocation->offset + offset;
        range.size = size;
        invalidateMappedMemoryRanges_(device_, 1, &range);
    }

    void freeMemory(const skgpu::VulkanBackendMemory &memory) override
    {
        auto *allocation = reinterpret_cast<RecoveryVulkanAllocation *>(memory);
        if (allocation == nullptr) {
            return;
        }
        if (freeMemory_ != nullptr && allocation->memory != VK_NULL_HANDLE) {
            freeMemory_(device_, allocation->memory, nullptr);
        }
        allocated_ -= allocation->size;
        used_ -= allocation->size;
        delete allocation;
    }

    std::pair<uint64_t, uint64_t> totalAllocatedAndUsedMemory() const override
    {
        return {allocated_, used_};
    }

private:
    VkResult allocate(const VkMemoryRequirements &requirements, VkMemoryPropertyFlags wanted, uint32_t allocationPropertyFlags, skgpu::VulkanBackendMemory *memory)
    {
        if (memory == nullptr || allocateMemory_ == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const uint32_t memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, wanted);
        if (memoryTypeIndex == UINT32_MAX) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkMemoryAllocateInfo allocateInfo = {};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = memoryTypeIndex;
        VkDeviceMemory vkMemory = VK_NULL_HANDLE;
        VkResult result = allocateMemory_(device_, &allocateInfo, nullptr, &vkMemory);
        if (result != VK_SUCCESS) {
            return result;
        }
        auto *allocation = new RecoveryVulkanAllocation();
        allocation->memory = vkMemory;
        allocation->size = requirements.size;
        allocation->offset = 0;
        const VkMemoryPropertyFlags properties = memoryProperties_.memoryTypes[memoryTypeIndex].propertyFlags;
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            allocation->flags |= skgpu::VulkanAlloc::kMappable_Flag;
        }
        if ((properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            allocation->flags |= skgpu::VulkanAlloc::kNoncoherent_Flag;
        }
        if ((allocationPropertyFlags & kLazyAllocation_AllocationPropertyFlag) != 0) {
            allocation->flags |= skgpu::VulkanAlloc::kLazilyAllocated_Flag;
        }
        *memory = reinterpret_cast<skgpu::VulkanBackendMemory>(allocation);
        allocated_ += requirements.size;
        used_ += requirements.size;
        return VK_SUCCESS;
    }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags wanted) const
    {
        for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) != 0 && (memoryProperties_.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return i;
            }
        }
        for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) != 0) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties_ = {};
    PFN_vkAllocateMemory allocateMemory_ = nullptr;
    PFN_vkFreeMemory freeMemory_ = nullptr;
    PFN_vkMapMemory mapMemory_ = nullptr;
    PFN_vkUnmapMemory unmapMemory_ = nullptr;
    PFN_vkFlushMappedMemoryRanges flushMappedMemoryRanges_ = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidateMappedMemoryRanges_ = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements_ = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements_ = nullptr;
    uint64_t allocated_ = 0;
    uint64_t used_ = 0;
};

template <typename T>
T vk_proc(const RecoveryVulkanSurface &state, const char *name, VkInstance instance, VkDevice device)
{
    return reinterpret_cast<T>(state.getProc(name, instance, device));
}

void recovery_vulkan_destroy_host_image(RecoveryVulkanSurface *state)
{
    if (state == nullptr) {
        return;
    }
    auto &hostImage = state->hostImage;
    hostImage.surface.reset();
    if (hostImage.data != nullptr) {
        if (auto unmapMemory = vk_proc<PFN_vkUnmapMemory>(*state, "vkUnmapMemory", state->instance, state->device)) {
            unmapMemory(state->device, hostImage.memory);
        }
        hostImage.data = nullptr;
    }
    if (hostImage.image != VK_NULL_HANDLE) {
        if (auto destroyImage = vk_proc<PFN_vkDestroyImage>(*state, "vkDestroyImage", state->instance, state->device)) {
            destroyImage(state->device, hostImage.image, nullptr);
        }
        hostImage.image = VK_NULL_HANDLE;
    }
    if (hostImage.memory != VK_NULL_HANDLE) {
        if (auto freeMemory = vk_proc<PFN_vkFreeMemory>(*state, "vkFreeMemory", state->instance, state->device)) {
            freeMemory(state->device, hostImage.memory, nullptr);
        }
        hostImage.memory = VK_NULL_HANDLE;
    }
    state->surface.reset();
    hostImage.rowBytes = 0;
    hostImage.memorySize = 0;
    hostImage.memoryCoherent = false;
    state->usesHostImageSurface = false;
}

void recovery_vulkan_destroy(RecoveryVulkanSurface *state)
{
    if (state == nullptr) {
        return;
    }
    state->surface.reset();
    recovery_vulkan_destroy_host_image(state);
    if (state->context) {
        state->context->releaseResourcesAndAbandonContext();
        state->context.reset();
    }
    if (state->device != VK_NULL_HANDLE) {
        if (auto destroyDevice = vk_proc<PFN_vkDestroyDevice>(*state, "vkDestroyDevice", state->instance, state->device)) {
            destroyDevice(state->device, nullptr);
        }
        state->device = VK_NULL_HANDLE;
    }
    if (state->halDevice != nullptr) {
        if (state->halDevice->close != nullptr) {
            state->halDevice->close(state->halDevice);
        }
        state->halDevice = nullptr;
    }
    if (state->instance != VK_NULL_HANDLE) {
        if (auto destroyInstance = vk_proc<PFN_vkDestroyInstance>(*state, "vkDestroyInstance", state->instance)) {
            destroyInstance(state->instance, nullptr);
        }
        state->instance = VK_NULL_HANDLE;
    }
    if (state->library != nullptr) {
        dlclose(state->library);
        state->library = nullptr;
    }
    delete state;
}

bool recovery_vulkan_load_system(RecoveryVulkanSurface *state)
{
    const char *explicitPath = std::getenv("SHAFT_VULKAN_SO");
    const char *paths[] = {
        explicitPath,
        std::getenv("FLUTTER_VULKAN_SO"),
        "libvulkan.so",
        "/system/lib64/libvulkan.so",
        "/vendor/lib64/libvulkan.so",
    };
    for (const char *path : paths) {
        if (path == nullptr || path[0] == '\0') {
            continue;
        }
        if (void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
            state->library = library;
            state->getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
            if (state->getInstanceProcAddr != nullptr) {
                state->createInstance = vk_proc<PFN_vkCreateInstance>(*state, "vkCreateInstance");
                state->enumerateInstanceExtensionProperties = vk_proc<PFN_vkEnumerateInstanceExtensionProperties>(*state, "vkEnumerateInstanceExtensionProperties");
                std::fprintf(stderr, "[CSkia:vulkan] loaded system Vulkan: %s\n", path);
                return true;
            }
            std::fprintf(stderr, "[CSkia:vulkan] vkGetInstanceProcAddr missing in %s\n", path);
            dlclose(library);
            state->library = nullptr;
        } else {
            std::fprintf(stderr, "[CSkia:vulkan] dlopen %s failed: %s\n", path, dlerror());
        }
    }
    return false;
}

bool recovery_vulkan_load_adreno_hal(RecoveryVulkanSurface *state)
{
    const char *explicitPath = std::getenv("SHAFT_VULKAN_SO");
    const char *flutterPath = std::getenv("FLUTTER_VULKAN_SO");
    if ((explicitPath != nullptr && explicitPath[0] != '\0') || (flutterPath != nullptr && flutterPath[0] != '\0')) {
        return false;
    }

    const char *paths[] = {
        "/vendor/lib64/hw/vulkan.adreno.so",
        "/vendor/lib/hw/vulkan.adreno.so",
    };
    for (const char *path : paths) {
        void *library = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (library == nullptr) {
            std::fprintf(stderr, "[CSkia:vulkan] dlopen %s failed: %s\n", path, dlerror());
            continue;
        }

        auto *hmi = reinterpret_cast<hw_module_t *>(dlsym(library, "HMI"));
        if (hmi == nullptr || hmi->methods == nullptr || hmi->methods->open == nullptr) {
            std::fprintf(stderr, "[CSkia:vulkan] HMI missing in %s\n", path);
            dlclose(library);
            continue;
        }

        hw_device_t *device = nullptr;
        const int rc = hmi->methods->open(hmi, "vulkan", &device);
        if (rc != 0 || device == nullptr) {
            std::fprintf(stderr, "[CSkia:vulkan] HAL open failed: %s rc=%d\n", path, rc);
            dlclose(library);
            continue;
        }

        auto *vulkanDevice = reinterpret_cast<hwvulkan_device_t *>(device);
        if (vulkanDevice->GetInstanceProcAddr == nullptr) {
            std::fprintf(stderr, "[CSkia:vulkan] HAL GetInstanceProcAddr is null: %s\n", path);
            if (device->close != nullptr) {
                device->close(device);
            }
            dlclose(library);
            continue;
        }

        state->library = library;
        state->halDevice = device;
        state->getInstanceProcAddr = vulkanDevice->GetInstanceProcAddr;
        state->createInstance = reinterpret_cast<PFN_vkCreateInstance>(state->getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        state->enumerateInstanceExtensionProperties = vulkanDevice->EnumerateInstanceExtensionProperties;
        if (state->createInstance == nullptr) {
            std::fprintf(stderr, "[CSkia:vulkan] failed to resolve vkCreateInstance from HAL GIPA: %s\n", path);
            if (device->close != nullptr) {
                device->close(device);
            }
            dlclose(library);
            state->library = nullptr;
            state->halDevice = nullptr;
            state->getInstanceProcAddr = nullptr;
            return false;
        }
        std::fprintf(stderr, "[CSkia:vulkan] loaded Adreno HAL: %s\n", path);
        return true;
    }
    return false;
}

bool recovery_vulkan_load_driver(RecoveryVulkanSurface *state)
{
    if (recovery_vulkan_load_adreno_hal(state)) {
        return true;
    }
    return recovery_vulkan_load_system(state);
}

void recovery_vulkan_log_missing_proc_group(
    const RecoveryVulkanSurface *state,
    const char *label,
    VkInstance instance,
    VkDevice device,
    const char *const *names,
    size_t count
) {
    size_t missing = 0;
    for (size_t index = 0; index < count; ++index) {
        if (state->getProc(names[index], instance, device) == nullptr) {
            std::fprintf(stderr, "[CSkia:vulkan] missing %s proc: %s\n", label, names[index]);
            ++missing;
        }
    }
    std::fprintf(stderr, "[CSkia:vulkan] missing %s procs=%zu/%zu\n", label, missing, count);
}

void recovery_vulkan_log_skia_proc_preflight(const RecoveryVulkanSurface *state)
{
    static const char *const globalProcs[] = {
        "vkCreateInstance",
        "vkEnumerateInstanceExtensionProperties",
        "vkEnumerateInstanceLayerProperties",
    };
    static const char *const instanceProcs[] = {
        "vkDestroyInstance",
        "vkEnumeratePhysicalDevices",
        "vkGetPhysicalDeviceFeatures",
        "vkGetPhysicalDeviceFormatProperties",
        "vkGetPhysicalDeviceImageFormatProperties",
        "vkGetPhysicalDeviceProperties",
        "vkGetPhysicalDeviceQueueFamilyProperties",
        "vkGetPhysicalDeviceMemoryProperties",
        "vkGetPhysicalDeviceSparseImageFormatProperties",
        "vkCreateDevice",
        "vkDestroyDevice",
        "vkEnumerateDeviceExtensionProperties",
        "vkEnumerateDeviceLayerProperties",
        "vkGetPhysicalDeviceFeatures2",
        "vkGetPhysicalDeviceProperties2",
        "vkGetPhysicalDeviceFormatProperties2",
        "vkGetPhysicalDeviceImageFormatProperties2",
        "vkGetPhysicalDeviceQueueFamilyProperties2",
        "vkGetPhysicalDeviceMemoryProperties2",
        "vkGetPhysicalDeviceSparseImageFormatProperties2",
        "vkGetPhysicalDeviceExternalBufferProperties",
    };
    static const char *const deviceProcs[] = {
        "vkGetDeviceQueue",
        "vkQueueSubmit",
        "vkQueueWaitIdle",
        "vkDeviceWaitIdle",
        "vkAllocateMemory",
        "vkFreeMemory",
        "vkMapMemory",
        "vkUnmapMemory",
        "vkFlushMappedMemoryRanges",
        "vkInvalidateMappedMemoryRanges",
        "vkGetDeviceMemoryCommitment",
        "vkBindBufferMemory",
        "vkBindImageMemory",
        "vkGetBufferMemoryRequirements",
        "vkGetImageMemoryRequirements",
        "vkGetImageSparseMemoryRequirements",
        "vkQueueBindSparse",
        "vkCreateFence",
        "vkDestroyFence",
        "vkResetFences",
        "vkGetFenceStatus",
        "vkWaitForFences",
        "vkCreateSemaphore",
        "vkDestroySemaphore",
        "vkCreateEvent",
        "vkDestroyEvent",
        "vkGetEventStatus",
        "vkSetEvent",
        "vkResetEvent",
        "vkCreateQueryPool",
        "vkDestroyQueryPool",
        "vkGetQueryPoolResults",
        "vkCreateBuffer",
        "vkDestroyBuffer",
        "vkCreateBufferView",
        "vkDestroyBufferView",
        "vkCreateImage",
        "vkDestroyImage",
        "vkGetImageSubresourceLayout",
        "vkCreateImageView",
        "vkDestroyImageView",
        "vkCreateShaderModule",
        "vkDestroyShaderModule",
        "vkCreatePipelineCache",
        "vkDestroyPipelineCache",
        "vkGetPipelineCacheData",
        "vkMergePipelineCaches",
        "vkCreateGraphicsPipelines",
        "vkCreateComputePipelines",
        "vkDestroyPipeline",
        "vkCreatePipelineLayout",
        "vkDestroyPipelineLayout",
        "vkCreateSampler",
        "vkDestroySampler",
        "vkCreateDescriptorSetLayout",
        "vkDestroyDescriptorSetLayout",
        "vkCreateDescriptorPool",
        "vkDestroyDescriptorPool",
        "vkResetDescriptorPool",
        "vkAllocateDescriptorSets",
        "vkFreeDescriptorSets",
        "vkUpdateDescriptorSets",
        "vkCreateFramebuffer",
        "vkDestroyFramebuffer",
        "vkCreateRenderPass",
        "vkDestroyRenderPass",
        "vkGetRenderAreaGranularity",
        "vkCreateCommandPool",
        "vkDestroyCommandPool",
        "vkResetCommandPool",
        "vkAllocateCommandBuffers",
        "vkFreeCommandBuffers",
        "vkBeginCommandBuffer",
        "vkEndCommandBuffer",
        "vkResetCommandBuffer",
        "vkCmdBindPipeline",
        "vkCmdSetViewport",
        "vkCmdSetScissor",
        "vkCmdSetLineWidth",
        "vkCmdSetDepthBias",
        "vkCmdSetBlendConstants",
        "vkCmdSetDepthBounds",
        "vkCmdSetStencilCompareMask",
        "vkCmdSetStencilWriteMask",
        "vkCmdSetStencilReference",
        "vkCmdBindDescriptorSets",
        "vkCmdBindIndexBuffer",
        "vkCmdBindVertexBuffers",
        "vkCmdDraw",
        "vkCmdDrawIndexed",
        "vkCmdDrawIndirect",
        "vkCmdDrawIndexedIndirect",
        "vkCmdDispatch",
        "vkCmdDispatchIndirect",
        "vkCmdCopyBuffer",
        "vkCmdCopyImage",
        "vkCmdBlitImage",
        "vkCmdCopyBufferToImage",
        "vkCmdCopyImageToBuffer",
        "vkCmdUpdateBuffer",
        "vkCmdFillBuffer",
        "vkCmdClearColorImage",
        "vkCmdClearDepthStencilImage",
        "vkCmdClearAttachments",
        "vkCmdResolveImage",
        "vkCmdSetEvent",
        "vkCmdResetEvent",
        "vkCmdWaitEvents",
        "vkCmdPipelineBarrier",
        "vkCmdBeginQuery",
        "vkCmdEndQuery",
        "vkCmdResetQueryPool",
        "vkCmdWriteTimestamp",
        "vkCmdCopyQueryPoolResults",
        "vkCmdPushConstants",
        "vkCmdBeginRenderPass",
        "vkCmdNextSubpass",
        "vkCmdEndRenderPass",
        "vkCmdExecuteCommands",
        "vkGetImageMemoryRequirements2",
        "vkGetBufferMemoryRequirements2",
        "vkGetImageSparseMemoryRequirements2",
        "vkBindBufferMemory2",
        "vkBindImageMemory2",
        "vkTrimCommandPool",
        "vkGetDescriptorSetLayoutSupport",
        "vkCreateSamplerYcbcrConversion",
        "vkDestroySamplerYcbcrConversion",
    };
    recovery_vulkan_log_missing_proc_group(state, "global", VK_NULL_HANDLE, VK_NULL_HANDLE, globalProcs, sizeof(globalProcs) / sizeof(globalProcs[0]));
    recovery_vulkan_log_missing_proc_group(state, "instance", state->instance, VK_NULL_HANDLE, instanceProcs, sizeof(instanceProcs) / sizeof(instanceProcs[0]));
    recovery_vulkan_log_missing_proc_group(state, "device", VK_NULL_HANDLE, state->device, deviceProcs, sizeof(deviceProcs) / sizeof(deviceProcs[0]));
}

uint32_t recovery_vulkan_find_memory_type(
    const RecoveryVulkanSurface *state,
    uint32_t typeBits,
    VkMemoryPropertyFlags wanted,
    VkMemoryPropertyFlags *actualProperties
) {
    if (state == nullptr) {
        return UINT32_MAX;
    }
    auto getMemoryProperties = vk_proc<PFN_vkGetPhysicalDeviceMemoryProperties>(*state, "vkGetPhysicalDeviceMemoryProperties", state->instance);
    if (getMemoryProperties == nullptr) {
        return UINT32_MAX;
    }
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    getMemoryProperties(state->physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags properties = memoryProperties.memoryTypes[i].propertyFlags;
        if ((typeBits & (1u << i)) != 0 && (properties & wanted) == wanted) {
            if (actualProperties != nullptr) {
                *actualProperties = properties;
            }
            return i;
        }
    }
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags properties = memoryProperties.memoryTypes[i].propertyFlags;
        if ((typeBits & (1u << i)) != 0 && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            if (actualProperties != nullptr) {
                *actualProperties = properties;
            }
            return i;
        }
    }
    return UINT32_MAX;
}

const char *recovery_vulkan_format_name(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8";
    case VK_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8";
    default:
        return "unknown";
    }
}

bool recovery_vulkan_create_host_image_surface_for_format(
    RecoveryVulkanSurface *state,
    RecoveryVulkanHostImage *hostImage,
    int width,
    int height,
    VkFormat format,
    SkColorType colorType
)
{
    auto createImage = vk_proc<PFN_vkCreateImage>(*state, "vkCreateImage", state->instance, state->device);
    auto getImageMemoryRequirements = vk_proc<PFN_vkGetImageMemoryRequirements>(*state, "vkGetImageMemoryRequirements", state->instance, state->device);
    auto allocateMemory = vk_proc<PFN_vkAllocateMemory>(*state, "vkAllocateMemory", state->instance, state->device);
    auto bindImageMemory = vk_proc<PFN_vkBindImageMemory>(*state, "vkBindImageMemory", state->instance, state->device);
    auto mapMemory = vk_proc<PFN_vkMapMemory>(*state, "vkMapMemory", state->instance, state->device);
    auto getImageSubresourceLayout = vk_proc<PFN_vkGetImageSubresourceLayout>(*state, "vkGetImageSubresourceLayout", state->instance, state->device);
    if (createImage == nullptr || getImageMemoryRequirements == nullptr || allocateMemory == nullptr ||
        bindImageMemory == nullptr || mapMemory == nullptr || getImageSubresourceLayout == nullptr) {
        return false;
    }

    auto getFormatProperties = vk_proc<PFN_vkGetPhysicalDeviceFormatProperties>(*state, "vkGetPhysicalDeviceFormatProperties", state->instance);
    if (getFormatProperties != nullptr) {
        VkFormatProperties properties = {};
        getFormatProperties(state->physicalDevice, format, &properties);
        std::fprintf(
            stderr,
            "[CSkia:vulkan] host format=%s linearFeatures=0x%x optimalFeatures=0x%x\n",
            recovery_vulkan_format_name(format),
            properties.linearTilingFeatures,
            properties.optimalTilingFeatures
        );
    }

    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        1,
    };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult imageResult = createImage(state->device, &imageInfo, nullptr, &hostImage->image);
    if (imageResult != VK_SUCCESS) {
        std::fprintf(stderr, "[CSkia:vulkan] host linear image create failed format=%s result=%d\n", recovery_vulkan_format_name(format), imageResult);
        return false;
    }

    VkMemoryRequirements memoryRequirements = {};
    getImageMemoryRequirements(state->device, hostImage->image, &memoryRequirements);
    VkMemoryPropertyFlags actualProperties = 0;
    const uint32_t memoryTypeIndex = recovery_vulkan_find_memory_type(
        state,
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &actualProperties
    );
    if (memoryTypeIndex == UINT32_MAX) {
        std::fprintf(stderr, "[CSkia:vulkan] no host-visible memory for host image format=%s\n", recovery_vulkan_format_name(format));
        return false;
    }

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = memoryTypeIndex;
    if (allocateMemory(state->device, &allocateInfo, nullptr, &hostImage->memory) != VK_SUCCESS) {
        std::fprintf(stderr, "[CSkia:vulkan] host image memory allocate failed format=%s\n", recovery_vulkan_format_name(format));
        return false;
    }
    hostImage->memorySize = memoryRequirements.size;
    hostImage->memoryCoherent = (actualProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    if (bindImageMemory(state->device, hostImage->image, hostImage->memory, 0) != VK_SUCCESS) {
        std::fprintf(stderr, "[CSkia:vulkan] host image bind failed format=%s\n", recovery_vulkan_format_name(format));
        return false;
    }
    if (mapMemory(state->device, hostImage->memory, 0, memoryRequirements.size, 0, &hostImage->data) != VK_SUCCESS) {
        std::fprintf(stderr, "[CSkia:vulkan] host image map failed format=%s\n", recovery_vulkan_format_name(format));
        return false;
    }
    std::memset(hostImage->data, 0, static_cast<size_t>(memoryRequirements.size));

    VkImageSubresource subresource = {};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.arrayLayer = 0;
    VkSubresourceLayout layout = {};
    getImageSubresourceLayout(state->device, hostImage->image, &subresource, &layout);
    hostImage->rowBytes = static_cast<size_t>(layout.rowPitch);

    GrVkImageInfo vkImageInfo;
    vkImageInfo.fImage = hostImage->image;
    vkImageInfo.fAlloc.fMemory = hostImage->memory;
    vkImageInfo.fAlloc.fOffset = 0;
    vkImageInfo.fAlloc.fSize = memoryRequirements.size;
    vkImageInfo.fAlloc.fFlags = skgpu::VulkanAlloc::kMappable_Flag |
        (hostImage->memoryCoherent ? 0 : skgpu::VulkanAlloc::kNoncoherent_Flag);
    // Skia's Vulkan backend expects externally wrapped render targets to be
    // reported as optimal here. The actual image stays linear and host-visible
    // so recovery can present the mapped memory directly after queue idle.
    vkImageInfo.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    vkImageInfo.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkImageInfo.fFormat = format;
    vkImageInfo.fImageUsageFlags = usage;
    vkImageInfo.fSampleCount = 1;
    vkImageInfo.fLevelCount = 1;
    vkImageInfo.fCurrentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    vkImageInfo.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto backendTexture = GrBackendTextures::MakeVk(width, height, vkImageInfo);
    if (!backendTexture.isValid()) {
        std::fprintf(stderr, "[CSkia:vulkan] host backend texture invalid format=%s\n", recovery_vulkan_format_name(format));
        return false;
    }

    SkSurfaceProps surfaceProps(0, kUnknown_SkPixelGeometry);
    hostImage->surface = SkSurfaces::WrapBackendTexture(
        state->context.get(),
        backendTexture,
        kTopLeft_GrSurfaceOrigin,
        1,
        colorType,
        SkColorSpace::MakeSRGB(),
        &surfaceProps
    );
    if (!hostImage->surface) {
        std::fprintf(stderr, "[CSkia:vulkan] host WrapBackendTexture failed format=%s\n", recovery_vulkan_format_name(format));
        return false;
    }
    return true;
}

bool recovery_vulkan_create_host_image_surface(RecoveryVulkanSurface *state, int width, int height)
{
    recovery_vulkan_destroy_host_image(state);
    if (!recovery_vulkan_create_host_image_surface_for_format(
            state,
            &state->hostImage,
            width,
            height,
            VK_FORMAT_R8G8B8A8_UNORM,
            kRGBA_8888_SkColorType
        )) {
        recovery_vulkan_destroy_host_image(state);
        return false;
    }
    state->surface = state->hostImage.surface;
    state->usesHostImageSurface = true;
    std::fprintf(stderr,
        "[CSkia:vulkan] host linear surface ok format=%s rowPitch=%zu coherent=%d\n",
        recovery_vulkan_format_name(VK_FORMAT_R8G8B8A8_UNORM),
        state->hostImage.rowBytes,
        state->hostImage.memoryCoherent ? 1 : 0);
    return true;
}

void recovery_vulkan_invalidate_host_image(RecoveryVulkanSurface *state)
{
    if (state == nullptr || state->hostImage.memoryCoherent) {
        return;
    }
    if (auto invalidate = vk_proc<PFN_vkInvalidateMappedMemoryRanges>(*state, "vkInvalidateMappedMemoryRanges", state->instance, state->device)) {
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = state->hostImage.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        invalidate(state->device, 1, &range);
    }
}
} // namespace

void *sk_recovery_vulkan_surface_create(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    auto *state = new RecoveryVulkanSurface();
    state->width = width;
    state->height = height;
    if (!recovery_vulkan_load_driver(state)) {
        std::fprintf(stderr, "[CSkia:vulkan] failed to load Vulkan driver\n");
        delete state;
        return nullptr;
    }

    std::fprintf(stderr, "[CSkia:vulkan] create surface begin size=%dx%d\n", width, height);
    // Some recovery-loaded Android Vulkan HALs expose vkEnumerateInstanceVersion
    // but never return from it. Skia needs Vulkan 1.1, so request that version
    // directly and let vkCreateInstance be the real capability check.
    uint32_t apiVersion = VK_API_VERSION_1_1;
    std::fprintf(stderr, "[CSkia:vulkan] apiVersion=%u\n", apiVersion);

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "ShaftRecoverySkia";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Shaft";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = apiVersion;

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    std::vector<VkExtensionProperties> availableInstanceExtensions;
    std::vector<const char *> enabledInstanceExtensions;
    if (state->enumerateInstanceExtensionProperties != nullptr) {
        uint32_t extensionCount = 0;
        std::fprintf(stderr, "[CSkia:vulkan] vkEnumerateInstanceExtensionProperties count begin\n");
        VkResult extensionResult = state->enumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        if (extensionResult == VK_SUCCESS && extensionCount > 0) {
            availableInstanceExtensions.resize(extensionCount);
            extensionResult = state->enumerateInstanceExtensionProperties(nullptr, &extensionCount, availableInstanceExtensions.data());
            if (extensionResult == VK_SUCCESS) {
                for (const auto &extension : availableInstanceExtensions) {
                    enabledInstanceExtensions.push_back(extension.extensionName);
                }
            } else {
                std::fprintf(stderr, "[CSkia:vulkan] vkEnumerateInstanceExtensionProperties list failed: %d\n", extensionResult);
            }
        } else if (extensionResult != VK_SUCCESS) {
            std::fprintf(stderr, "[CSkia:vulkan] vkEnumerateInstanceExtensionProperties count failed: %d\n", extensionResult);
        }
        std::fprintf(stderr, "[CSkia:vulkan] instanceExtensionCount=%zu\n", enabledInstanceExtensions.size());
    }
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledInstanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = enabledInstanceExtensions.empty() ? nullptr : enabledInstanceExtensions.data();

    auto createInstance = state->createInstance != nullptr ? state->createInstance : vk_proc<PFN_vkCreateInstance>(*state, "vkCreateInstance");
    std::fprintf(stderr, "[CSkia:vulkan] vkCreateInstance begin\n");
    if (createInstance == nullptr || createInstance(&instanceInfo, nullptr, &state->instance) != VK_SUCCESS) {
        std::fprintf(stderr, "[CSkia:vulkan] vkCreateInstance failed\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }
    std::fprintf(stderr, "[CSkia:vulkan] vkCreateInstance ok\n");

    auto enumeratePhysicalDevices = vk_proc<PFN_vkEnumeratePhysicalDevices>(*state, "vkEnumeratePhysicalDevices", state->instance);
    auto getPhysicalDeviceQueueFamilyProperties = vk_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(*state, "vkGetPhysicalDeviceQueueFamilyProperties", state->instance);
    if (enumeratePhysicalDevices == nullptr || getPhysicalDeviceQueueFamilyProperties == nullptr) {
        std::fprintf(stderr, "[CSkia:vulkan] failed to resolve physical-device procs\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }

    uint32_t physicalDeviceCount = 0;
    std::fprintf(stderr, "[CSkia:vulkan] vkEnumeratePhysicalDevices count begin\n");
    if (enumeratePhysicalDevices(state->instance, &physicalDeviceCount, nullptr) != VK_SUCCESS || physicalDeviceCount == 0) {
        std::fprintf(stderr, "[CSkia:vulkan] no physical devices\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }
    std::fprintf(stderr, "[CSkia:vulkan] physicalDeviceCount=%u\n", physicalDeviceCount);
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    if (enumeratePhysicalDevices(state->instance, &physicalDeviceCount, physicalDevices.data()) != VK_SUCCESS) {
        recovery_vulkan_destroy(state);
        return nullptr;
    }

    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        uint32_t queueFamilyCount = 0;
        getPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        getPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
        for (uint32_t index = 0; index < queueFamilyCount; ++index) {
            if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                state->physicalDevice = physicalDevice;
                state->queueFamilyIndex = index;
                break;
            }
        }
        if (state->physicalDevice != VK_NULL_HANDLE) {
            break;
        }
    }
    if (state->physicalDevice == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[CSkia:vulkan] no graphics queue family\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = state->queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceFeatures deviceFeatures = {};
    VkPhysicalDeviceImageCompressionControlFeaturesEXT compressFeatures = {};
    compressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT;
    VkPhysicalDevice16BitStorageFeatures storage16Features = {};
    storage16Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    storage16Features.pNext = &compressFeatures;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures = {};
    ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrFeatures.pNext = &storage16Features;
    VkPhysicalDeviceFeatures2 supportedFeatures2 = {};
    supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures2.pNext = &ycbcrFeatures;
    bool useFeatures2 = false;
    std::vector<VkExtensionProperties> availableDeviceExtensions;
    std::vector<const char *> enabledDeviceExtensions;
    auto enumerateDeviceExtensionProperties = vk_proc<PFN_vkEnumerateDeviceExtensionProperties>(*state, "vkEnumerateDeviceExtensionProperties", state->instance);
    if (enumerateDeviceExtensionProperties != nullptr) {
        uint32_t deviceExtensionCount = 0;
        if (enumerateDeviceExtensionProperties(state->physicalDevice, nullptr, &deviceExtensionCount, nullptr) == VK_SUCCESS && deviceExtensionCount > 0) {
            availableDeviceExtensions.resize(deviceExtensionCount);
            if (enumerateDeviceExtensionProperties(state->physicalDevice, nullptr, &deviceExtensionCount, availableDeviceExtensions.data()) == VK_SUCCESS) {
                auto hasDeviceExtension = [&](const char *name) {
                    for (const auto &extension : availableDeviceExtensions) {
                        if (std::strcmp(extension.extensionName, name) == 0) {
                            return true;
                        }
                    }
                    return false;
                };
                const char *wantedDeviceExtensions[] = {
                    "VK_ANDROID_external_memory_android_hardware_buffer",
                    "VK_KHR_sampler_ycbcr_conversion",
                    "VK_KHR_external_memory",
                    "VK_EXT_queue_family_foreign",
                    "VK_KHR_dedicated_allocation",
                    "VK_KHR_external_fence_fd",
                    "VK_KHR_external_fence",
                    "VK_KHR_external_semaphore_fd",
                    "VK_KHR_external_semaphore",
                    "VK_KHR_bind_memory2",
                    "VK_KHR_get_memory_requirements2",
                    "VK_KHR_external_memory_fd",
                    "VK_EXT_pipeline_creation_feedback",
                    "VK_EXT_image_compression_control",
                    "VK_KHR_maintenance1",
                    "VK_KHR_maintenance2",
                    "VK_KHR_maintenance3",
                    "VK_KHR_16bit_storage",
                    "VK_KHR_storage_buffer_storage_class",
                    "VK_KHR_multiview",
                    "VK_KHR_variable_pointers",
                    "VK_KHR_shader_draw_parameters",
                    "VK_KHR_descriptor_update_template",
                    "VK_KHR_device_group",
                    "VK_KHR_create_renderpass2",
                    "VK_KHR_depth_stencil_resolve",
                    "VK_KHR_separate_depth_stencil_layouts",
                    "VK_KHR_image_format_list",
                    "VK_KHR_imageless_framebuffer",
                    "VK_KHR_sampler_mirror_clamp_to_edge",
                    "VK_ANDROID_native_buffer",
                    "VK_KHR_driver_properties",
                    "VK_KHR_timeline_semaphore",
                    "VK_KHR_synchronization2",
                    "VK_KHR_copy_commands2",
                    "VK_KHR_8bit_storage",
                    "VK_KHR_shader_float16_int8",
                    "VK_KHR_uniform_buffer_standard_layout",
                    "VK_EXT_scalar_block_layout",
                    "VK_EXT_host_query_reset",
                    "VK_EXT_shader_demote_to_helper_invocation",
                    "VK_KHR_shader_subgroup_extended_types",
                    "VK_KHR_shader_non_semantic_info",
                    "VK_KHR_shader_terminate_invocation",
                    "VK_KHR_spirv_1_4",
                    "VK_KHR_shader_float_controls",
                    "VK_EXT_descriptor_indexing",
                    "VK_KHR_draw_indirect_count",
                    "VK_KHR_zero_initialize_workgroup_memory",
                    "VK_KHR_shader_integer_dot_product",
                    "VK_EXT_4444_formats",
                    "VK_EXT_texture_compression_astc_hdr",
                    "VK_EXT_private_data",
                    "VK_EXT_image_robustness",
                    "VK_EXT_pipeline_creation_cache_control",
                    "VK_EXT_load_store_op_none",
                    "VK_EXT_sampler_filter_minmax",
                    "VK_EXT_depth_range_unrestricted",
                };
                for (const char *extension : wantedDeviceExtensions) {
                    if (hasDeviceExtension(extension)) {
                        enabledDeviceExtensions.push_back(extension);
                    }
                }
            }
        }
    }
    std::fprintf(stderr, "[CSkia:vulkan] deviceExtensionCount=%zu\n", enabledDeviceExtensions.size());
    if (!enabledDeviceExtensions.empty()) {
        std::fprintf(stderr, "[CSkia:vulkan] enabledDeviceExtensions:");
        for (const char *extension : enabledDeviceExtensions) {
            std::fprintf(stderr, " %s", extension);
        }
        std::fprintf(stderr, "\n");
    }

    auto getPhysicalDeviceFeatures2 = vk_proc<PFN_vkGetPhysicalDeviceFeatures2>(*state, "vkGetPhysicalDeviceFeatures2", state->instance);
    if (getPhysicalDeviceFeatures2 != nullptr) {
        getPhysicalDeviceFeatures2(state->physicalDevice, &supportedFeatures2);
        useFeatures2 = true;
        std::fprintf(
            stderr,
            "[CSkia:vulkan] features2 ycbcr=%u storage16=%u compressionControl=%u\n",
            static_cast<unsigned>(ycbcrFeatures.samplerYcbcrConversion),
            static_cast<unsigned>(storage16Features.storageBuffer16BitAccess),
            static_cast<unsigned>(compressFeatures.imageCompressionControl)
        );
    }

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = useFeatures2 ? &supportedFeatures2 : nullptr;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pEnabledFeatures = useFeatures2 ? nullptr : &deviceFeatures;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions.empty() ? nullptr : enabledDeviceExtensions.data();

    auto createDevice = vk_proc<PFN_vkCreateDevice>(*state, "vkCreateDevice", state->instance);
    std::fprintf(stderr, "[CSkia:vulkan] vkCreateDevice begin queueFamily=%u\n", state->queueFamilyIndex);
    if (createDevice == nullptr || createDevice(state->physicalDevice, &deviceInfo, nullptr, &state->device) != VK_SUCCESS) {
        std::fprintf(stderr, "[CSkia:vulkan] vkCreateDevice failed\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }
    std::fprintf(stderr, "[CSkia:vulkan] vkCreateDevice ok\n");
    state->getDeviceProcAddr = vk_proc<PFN_vkGetDeviceProcAddr>(*state, "vkGetDeviceProcAddr", state->instance);
    auto getDeviceQueue = vk_proc<PFN_vkGetDeviceQueue>(*state, "vkGetDeviceQueue", state->instance, state->device);
    if (getDeviceQueue == nullptr) {
        std::fprintf(stderr, "[CSkia:vulkan] vkGetDeviceQueue missing\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }
    getDeviceQueue(state->device, state->queueFamilyIndex, 0, &state->queue);
    std::fprintf(stderr, "[CSkia:vulkan] vkGetDeviceQueue ok\n");

    std::fprintf(stderr, "[CSkia:vulkan] VulkanExtensions init begin\n");
    state->extensions.init(
        [state](const char *name, VkInstance instance, VkDevice device) {
            return state->getProc(name, instance, device);
        },
        state->instance,
        state->physicalDevice,
        static_cast<uint32_t>(enabledInstanceExtensions.size()),
        enabledInstanceExtensions.empty() ? nullptr : enabledInstanceExtensions.data(),
        static_cast<uint32_t>(enabledDeviceExtensions.size()),
        enabledDeviceExtensions.empty() ? nullptr : enabledDeviceExtensions.data()
    );
    std::fprintf(stderr, "[CSkia:vulkan] VulkanExtensions init ok\n");

    skgpu::VulkanBackendContext backendContext;
    backendContext.fInstance = state->instance;
    backendContext.fPhysicalDevice = state->physicalDevice;
    backendContext.fDevice = state->device;
    backendContext.fQueue = state->queue;
    backendContext.fGraphicsQueueIndex = state->queueFamilyIndex;
    backendContext.fMaxAPIVersion = apiVersion;
    backendContext.fVkExtensions = &state->extensions;
    backendContext.fDeviceFeatures = useFeatures2 ? nullptr : &deviceFeatures;
    backendContext.fDeviceFeatures2 = useFeatures2 ? &supportedFeatures2 : nullptr;
    state->memoryAllocator = sk_make_sp<RecoveryVulkanMemoryAllocator>(*state);
    backendContext.fMemoryAllocator = state->memoryAllocator;
    backendContext.fGetProc = [state](const char *name, VkInstance instance, VkDevice device) {
        return state->getProc(name, instance, device);
    };

    std::fprintf(stderr, "[CSkia:vulkan] GrDirectContexts::MakeVulkan begin\n");
    recovery_vulkan_log_skia_proc_preflight(state);
    state->context = GrDirectContexts::MakeVulkan(backendContext);
    if (!state->context) {
        std::fprintf(stderr, "[CSkia:vulkan] GrDirectContexts::MakeVulkan failed\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }
    std::fprintf(stderr, "[CSkia:vulkan] GrDirectContexts::MakeVulkan ok\n");

    const char *surfaceMode = std::getenv("SHAFT_VULKAN_SURFACE");
    const bool preferOptimalReadback = surfaceMode != nullptr && std::strcmp(surfaceMode, "optimal-readback") == 0;
    if (!preferOptimalReadback) {
        std::fprintf(stderr, "[CSkia:vulkan] surface=host-linear begin\n");
        if (recovery_vulkan_create_host_image_surface(state, width, height)) {
            return state;
        }
        std::fprintf(stderr, "[CSkia:vulkan] host linear surface unavailable; fallback surface=optimal-readback\n");
    } else {
        std::fprintf(stderr, "[CSkia:vulkan] surface=optimal-readback\n");
    }

    auto imageInfo = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    std::fprintf(stderr, "[CSkia:vulkan] SkSurfaces::RenderTarget begin\n");
    state->surface = SkSurfaces::RenderTarget(
        state->context.get(),
        skgpu::Budgeted::kNo,
        imageInfo,
        0,
        kTopLeft_GrSurfaceOrigin,
        nullptr
    );
    if (!state->surface) {
        std::fprintf(stderr, "[CSkia:vulkan] SkSurfaces::RenderTarget failed\n");
        recovery_vulkan_destroy(state);
        return nullptr;
    }
    std::fprintf(stderr, "[CSkia:vulkan] SkSurfaces::RenderTarget ok\n");

    return state;
}

SkSurface_sp sk_recovery_vulkan_surface_get_surface(void *surface)
{
    auto *state = static_cast<RecoveryVulkanSurface *>(surface);
    if (state == nullptr) {
        return nullptr;
    }
    return state->surface;
}

bool sk_recovery_vulkan_surface_flush_and_get_pixels(void *surface, void **pixels, size_t *rowBytes)
{
    auto *state = static_cast<RecoveryVulkanSurface *>(surface);
    if (pixels != nullptr) {
        *pixels = nullptr;
    }
    if (rowBytes != nullptr) {
        *rowBytes = 0;
    }
    if (state == nullptr || !state->surface || !state->usesHostImageSurface) {
        return false;
    }
    auto &hostImage = state->hostImage;
    if (!hostImage.surface || hostImage.data == nullptr) {
        return false;
    }

    const bool profile = recovery_profile_enabled();
    const int64_t t0 = profile ? recovery_now_us() : 0;
    state->context->flush(hostImage.surface.get());
    state->context->submit(GrSyncCpu::kNo);
    const int64_t t1 = profile ? recovery_now_us() : 0;
    auto queueWaitIdle = vk_proc<PFN_vkQueueWaitIdle>(*state, "vkQueueWaitIdle", state->instance, state->device);
    if (queueWaitIdle == nullptr || queueWaitIdle(state->queue) != VK_SUCCESS) {
        return false;
    }
    const int64_t t2 = profile ? recovery_now_us() : 0;
    recovery_vulkan_invalidate_host_image(state);
    const int64_t t3 = profile ? recovery_now_us() : 0;
    if (pixels != nullptr) {
        *pixels = hostImage.data;
    }
    if (rowBytes != nullptr) {
        *rowBytes = hostImage.rowBytes;
    }
    if (profile) {
        std::fprintf(
            stderr,
            "[CSkia:vulkan:profile] flush_us=%lld wait_us=%lld invalidate_us=%lld total_us=%lld mapped=1\n",
            static_cast<long long>(t1 - t0),
            static_cast<long long>(t2 - t1),
            static_cast<long long>(t3 - t2),
            static_cast<long long>(t3 - t0)
        );
    }
    return true;
}

bool sk_recovery_vulkan_surface_flush_and_read(void *surface, void *pixels, size_t rowBytes)
{
    auto *state = static_cast<RecoveryVulkanSurface *>(surface);
    if (state == nullptr || !state->surface || pixels == nullptr || rowBytes < static_cast<size_t>(state->width) * 4) {
        return false;
    }
    const bool profile = recovery_profile_enabled();
    const int64_t t0 = profile ? recovery_now_us() : 0;
    skgpu::ganesh::FlushAndSubmit(state->surface);
    const int64_t t1 = profile ? recovery_now_us() : 0;
    auto imageInfo = SkImageInfo::Make(state->width, state->height, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    const bool ok = state->surface->readPixels(imageInfo, pixels, rowBytes, 0, 0);
    if (profile) {
        const int64_t t2 = recovery_now_us();
        std::fprintf(
            stderr,
            "[CSkia:vulkan:profile] flush_us=%lld read_us=%lld total_us=%lld ok=%d\n",
            static_cast<long long>(t1 - t0),
            static_cast<long long>(t2 - t1),
            static_cast<long long>(t2 - t0),
            ok ? 1 : 0
        );
    }
    return ok;
}

void sk_recovery_vulkan_surface_destroy(void *surface)
{
    recovery_vulkan_destroy(static_cast<RecoveryVulkanSurface *>(surface));
}
#endif
