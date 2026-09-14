#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

extern "C" __declspec(dllexport) int RunVulkanModuleProbe()
{
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "late Vulkan engine module probe";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult result = vkCreateInstance(&create, nullptr, &instance);
    if (result == VK_SUCCESS) vkDestroyInstance(instance, nullptr);
    return result == VK_SUCCESS ? 0 : 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, void *) { return TRUE; }
