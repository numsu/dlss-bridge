#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    HMODULE injector = argc > 1 ? LoadLibraryA(argv[1]) : nullptr;
    if (argc > 1) std::printf("injector=%p error=%lu\n", (void *)injector, GetLastError());
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    if (!vk) { std::printf("no vulkan-1.dll: %lu\n", GetLastError()); return 2; }
    wchar_t vkpath[MAX_PATH] = {}; GetModuleFileNameW(vk, vkpath, MAX_PATH);
    std::printf("vulkan=%ls\n", vkpath);
    if (injector) Sleep(250); // let an injected hook host observe the just-loaded facade
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        GetProcAddress(vk, "vkEnumerateInstanceLayerProperties"));
    auto create = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(vk, "vkCreateInstance"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(GetProcAddress(vk, "vkDestroyInstance"));
    if (!enumerate || !create || !destroy) return 3;
    uint32_t count = 0; VkResult r = enumerate(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    if (r == VK_SUCCESS) r = enumerate(&count, layers.data());
    bool found = false;
    for (const auto &layer : layers) {
        std::printf("layer=%s\n", layer.layerName);
        if (std::strcmp(layer.layerName, "VK_LAYER_dlss5_vk_bridge") == 0) found = true;
    }
    if (!found && argc == 1) { std::printf("bridge layer not enumerated\n"); return 4; }
    const char *enabled[] = { "VK_LAYER_dlss5_vk_bridge" };
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "dlss bridge probe"; ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai; ci.enabledLayerCount = found ? 1u : 0u; ci.ppEnabledLayerNames = found ? enabled : nullptr;
    VkInstance instance = VK_NULL_HANDLE; r = create(&ci, nullptr, &instance);
    std::printf("create=0x%08x instance=%p\n", (unsigned)r, (void *)instance);
    if (instance) destroy(instance, nullptr);
    return r == VK_SUCCESS ? 0 : 5;
}
