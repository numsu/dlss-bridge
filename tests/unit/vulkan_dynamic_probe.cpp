#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>

int main()
{
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) return 2;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(
        GetProcAddress(loader, "vkCreateInstance"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
        GetProcAddress(loader, "vkDestroyInstance"));
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "dynamic Vulkan loader probe";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult result = create ? create(&info, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
    std::printf("dynamic_loader_result=0x%08x\n", static_cast<unsigned>(result));
    if (result == VK_SUCCESS && destroy) destroy(instance, nullptr);
    FreeLibrary(loader);
    return result == VK_SUCCESS ? 0 : 1;
}
