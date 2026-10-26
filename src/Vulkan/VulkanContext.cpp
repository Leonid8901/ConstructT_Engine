#include "Vulkan/VulkanContext.h"
#include "Core.h"
#include "Platform/Window.h"
#include "Vulkan/VkCheck.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <set>

namespace ce::core {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kRtExtensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                         VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                         VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};

#ifdef NDEBUG
constexpr bool kWantValidation = false;
#else
constexpr bool kWantValidation = true;
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* tag = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "[VK ERROR] " : "[VK WARN] ";
    std::cerr << tag << data->pMessage << '\n';
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT DebugMessengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice d) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(d, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &count, exts.data());
    return exts;
}

bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

VulkanContext::VulkanContext(const Window& window, const std::string& devicePreference) {
    CreateInstance();
    SetupDebugMessenger();
    m_surface = window.CreateSurface(m_instance);
    PickPhysicalDevice(devicePreference);
    CreateDevice();
}

VulkanContext::~VulkanContext() {
    if (m_device)
        vkDestroyDevice(m_device, nullptr);
    if (m_surface)
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy)
            destroy(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance)
        vkDestroyInstance(m_instance, nullptr);
}

std::vector<uint32_t> VulkanContext::UniqueFamilies() const {
    std::set<uint32_t> s{m_families.graphics, m_families.present, m_families.compute, m_families.transfer};
    return {s.begin(), s.end()};
}

void VulkanContext::CreateInstance() {
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3)
        throw std::runtime_error("Загрузчик Vulkan версии " + std::to_string(VK_API_VERSION_MAJOR(loaderVersion)) + "." +
                                 std::to_string(VK_API_VERSION_MINOR(loaderVersion)) +
                                 ", нужен 1.3. Обновите драйвер видеокарты (на Linux — пакет Mesa или драйвер NVIDIA).");

    m_validation = kWantValidation && HasLayer(kValidationLayer);
    if (kWantValidation && !m_validation)
        Log("Validation layer not found (install Vulkan SDK), running without it");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "CustomEngine";
    app.pEngineName = "CustomEngine";
    app.apiVersion = VK_API_VERSION_1_3;

    auto extensions = Window::RequiredInstanceExtensions();
    if (m_validation)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugInfo = DebugMessengerInfo();
    if (m_validation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &kValidationLayer;
        info.pNext = &debugInfo;
    }
    const VkResult r = vkCreateInstance(&info, nullptr, &m_instance);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER || r == VK_ERROR_INITIALIZATION_FAILED)
        throw std::runtime_error("Не найден драйвер Vulkan. Установите драйвер видеокарты с поддержкой Vulkan 1.3.");
    VK_CHECK(r);
}

void VulkanContext::SetupDebugMessenger() {
    if (!m_validation)
        return;
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!create)
        return;
    VkDebugUtilsMessengerCreateInfoEXT info = DebugMessengerInfo();
    VK_CHECK(create(m_instance, &info, nullptr, &m_debugMessenger));
}

bool VulkanContext::IsDeviceSuitable(VkPhysicalDevice device, QueueFamilies& out, std::string& reason) const {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        reason = "драйвер даёт Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
                 std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + ", нужен 1.3 (обновите драйвер; если не помогает — GPU слишком старый)";
        return false;
    }
    if (!HasExtension(DeviceExtensions(device), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        reason = "нет VK_KHR_swapchain (не умеет выводить на экран)";
        return false;
    }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(device, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) {
        reason = "нет dynamicRendering/synchronization2";
        return false;
    }
    // Bindless-текстуры и адреса буферов: в Vulkan 1.3 обязательны, но проверяем явно
    if (!f12.bufferDeviceAddress || !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound ||
        !f12.descriptorBindingSampledImageUpdateAfterBind || !f12.descriptorBindingUpdateUnusedWhilePending ||
        !f12.descriptorBindingVariableDescriptorCount || !f12.shaderSampledImageArrayNonUniformIndexing) {
        reason = "нет descriptor indexing / buffer device address";
        return false;
    }

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

    QueueFamilies r;
    for (uint32_t i = 0; i < familyCount; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
        bool graphics = families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
        if (graphics && present) {
            r.graphics = r.present = i;
            break;
        }
        if (graphics && r.graphics == UINT32_MAX) r.graphics = i;
        if (present && r.present == UINT32_MAX) r.present = i;
    }
    if (r.graphics == UINT32_MAX || r.present == UINT32_MAX) {
        reason = r.graphics == UINT32_MAX ? "нет графической очереди" : "не может выводить в это окно";
        return false;
    }

    // GPU-лейны UniX: ищем выделенные compute и transfer семьи
    r.compute = r.graphics;
    r.transfer = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        VkQueueFlags f = families[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && r.compute == r.graphics)
            r.compute = i;
        if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
            r.transfer == UINT32_MAX)
            r.transfer = i;
    }
    if (r.transfer == UINT32_MAX)
        r.transfer = r.compute;

    uint32_t formatCount = 0, modeCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &modeCount, nullptr);
    if (formatCount == 0 || modeCount == 0) {
        reason = "нет форматов/режимов показа для окна";
        return false;
    }

    out = r;
    return true;
}

void VulkanContext::PickPhysicalDevice(const std::string& preference) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0)
        throw std::runtime_error("Vulkan не нашёл ни одного GPU. Установите или обновите драйвер видеокарты.");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string pref = lower(preference);
    const bool byIndex = !pref.empty() && std::all_of(pref.begin(), pref.end(), [](char c) { return c >= '0' && c <= '9'; });

    long long best = -1;
    bool preferredFound = false;
    std::string rejected;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDevice d = devices[i];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(d, &mem);
        uint64_t vram = 0;
        for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
            if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                vram = std::max<uint64_t>(vram, mem.memoryHeaps[h].size);

        QueueFamilies families;
        std::string reason;
        const bool ok = IsDeviceSuitable(d, families, reason);
        Log("GPU " + std::to_string(i) + ": " + props.deviceName + " (" + GpuVendorName(props.vendorID) + ", " +
            std::to_string(vram >> 20) + " МБ)" + (ok ? "" : " — не подходит: " + reason));
        if (!ok) {
            rejected += std::string("\n  ") + props.deviceName + ": " + reason;
            continue;
        }
        long long score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3000
                          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2000
                          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 1000
                                                                                       : 0;
        score += static_cast<long long>(std::min<uint64_t>(vram >> 30, 999));  // ГБ VRAM — при равном типе
        const bool preferred = pref != "auto" && !pref.empty() &&
                               (byIndex ? std::to_string(i) == pref : lower(props.deviceName).find(pref) != std::string::npos);
        if (preferred) {
            score += 1000000;
            preferredFound = true;
        }
        if (score > best) {
            best = score;
            m_physicalDevice = d;
            m_families = families;
        }
    }
    if (pref != "auto" && !pref.empty() && !preferredFound)
        Log("engine.json: GPU \"" + preference + "\" не найден или не подходит — выбран автоматически");
    if (!m_physicalDevice)
        throw std::runtime_error("Нет подходящего GPU (нужен Vulkan 1.3 c dynamic rendering и synchronization2):" + rejected);

    // Поддержка ray query
    auto exts = DeviceExtensions(m_physicalDevice);
    bool rtExts = std::all_of(std::begin(kRtExtensions), std::end(kRtExtensions),
                              [&](const char* e) { return HasExtension(exts, e); });
    if (rtExts) {
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        as.pNext = &rq;
        f12.pNext = &as;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(m_physicalDevice, &f2);
        m_rtSupported = rq.rayQuery && as.accelerationStructure && f12.bufferDeviceAddress;
    }

    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    if (m_rtSupported)
        p2.pNext = &m_asProps;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);
    m_props = p2.properties;
    FillInfo();

    Log(std::string("GPU: ") + m_props.deviceName + " | Vulkan " + std::to_string(VK_API_VERSION_MAJOR(m_props.apiVersion)) +
        "." + std::to_string(VK_API_VERSION_MINOR(m_props.apiVersion)) +
        " | ray query: " + (m_rtSupported ? "yes" : "no"));
}

void VulkanContext::FillInfo() {
    GpuInfo& g = m_info;
    g.vendorId = m_props.vendorID;
    g.deviceId = m_props.deviceID;
    g.vendor = GpuVendorName(m_props.vendorID);
    g.name = m_props.deviceName;
    g.apiVersion = m_props.apiVersion;
    g.rayQuery = m_rtSupported;
    switch (m_props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: g.type = GpuClass::Discrete; break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: g.type = GpuClass::Integrated; break;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: g.type = GpuClass::Virtual; break;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: g.type = GpuClass::Software; break;
    default: g.type = GpuClass::Unknown; break;
    }

    // Драйвер (Vulkan 1.2 core): различает RADV / AMDVLK / AMD proprietary, NVIDIA / NVK, ANV и т.д.
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);
    g.driver = std::string(drv.driverName) + (drv.driverInfo[0] ? std::string(" ") + drv.driverInfo : "");
    if (drv.driverID == VK_DRIVER_ID_MESA_LLVMPIPE || drv.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER)
        g.type = GpuClass::Software;  // некоторые сборки помечают себя не как CPU

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mem);
    for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
        if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            g.vram = std::max<uint64_t>(g.vram, mem.memoryHeaps[h].size);
    for (uint32_t t = 0; t < mem.memoryTypeCount; ++t) {
        const VkMemoryPropertyFlags f = mem.memoryTypes[t].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            g.hostVisibleVram = std::max<uint64_t>(g.hostVisibleVram, mem.memoryHeaps[mem.memoryTypes[t].heapIndex].size);
    }
}

void VulkanContext::CreateDevice() {
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32_t family : UniqueFamilies()) {
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queueInfos.push_back(q);
    }

    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    as.accelerationStructure = VK_TRUE;
    as.pNext = &rq;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    if (m_rtSupported)
        f13.pNext = &as;

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;  // скиннинг и RT берут буферы по адресу
    // Bindless: один массив всех текстур, индекс текстуры лежит в данных инстанса
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.pNext = &f13;

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supported);
    m_anisotropy = supported.samplerAnisotropy == VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.samplerAnisotropy = m_anisotropy ? VK_TRUE : VK_FALSE;
    f2.pNext = &f12;

    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (m_rtSupported)
        extensions.insert(extensions.end(), std::begin(kRtExtensions), std::end(kRtExtensions));

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f2;
    info.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    info.pQueueCreateInfos = queueInfos.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateDevice(m_physicalDevice, &info, nullptr, &m_device));
}

} // namespace ce::core
