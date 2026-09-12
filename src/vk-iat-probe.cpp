#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
#define RESOLVE_G(name) reinterpret_cast<PFN_vk##name>(vkGetInstanceProcAddr(instance, "vk" #name))
#define RESOLVE_D(name) reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name))
int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    HMODULE injector = argc > 1 ? LoadLibraryA(argv[1]) : nullptr;
    std::printf("injector=%p error=%lu\n", (void *)injector, GetLastError());
    Sleep(250);
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    std::printf("gipa_create=%p imported_create=%p\n", (void *)createInstance, (void *)&vkCreateInstance);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "dlss bridge iat probe"; ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &ai;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = createInstance ? createInstance(&ci, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
    std::printf("create_instance=0x%08x instance=%p\n", (unsigned)r, (void *)instance);
    if (r != VK_SUCCESS) return 2;
    auto enumPhys=RESOLVE_G(EnumeratePhysicalDevices);
    auto getQProps=RESOLVE_G(GetPhysicalDeviceQueueFamilyProperties);
    auto createDevice=RESOLVE_G(CreateDevice);
    auto destroyInstance=RESOLVE_G(DestroyInstance);
    auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetInstanceProcAddr(instance,"vkGetDeviceProcAddr"));
    uint32_t count=0; r=enumPhys(instance,&count,nullptr); std::vector<VkPhysicalDevice> phys(count);
    if(r==VK_SUCCESS) r=enumPhys(instance,&count,phys.data());
    std::printf("physical_devices=%u result=0x%08x create_device_fn=%p gdpa=%p\n",count,(unsigned)r,(void*)createDevice,(void*)gdpa);
    if(r!=VK_SUCCESS||count==0||!createDevice||!gdpa){destroyInstance(instance,nullptr);return 3;}
    uint32_t qcount=0; getQProps(phys[0],&qcount,nullptr); std::vector<VkQueueFamilyProperties> qp(qcount);
    getQProps(phys[0],&qcount,qp.data()); uint32_t family=0;
    for(uint32_t i=0;i<qcount;++i)if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    float priority=1.0f; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    VkDevice device=VK_NULL_HANDLE;r=createDevice(phys[0],&di,nullptr,&device);
    std::printf("create_device=0x%08x device=%p family=%u\n",(unsigned)r,(void*)device,family);
    if(r!=VK_SUCCESS){destroyInstance(instance,nullptr);return 4;}
    auto getQueue=RESOLVE_D(GetDeviceQueue); auto createPool=RESOLVE_D(CreateCommandPool);
    auto allocCmd=RESOLVE_D(AllocateCommandBuffers); auto beginCmd=RESOLVE_D(BeginCommandBuffer);
    auto endCmd=RESOLVE_D(EndCommandBuffer); auto submit=RESOLVE_D(QueueSubmit);
    auto queueWait=RESOLVE_D(QueueWaitIdle); auto destroyPool=RESOLVE_D(DestroyCommandPool);
    auto destroyDevice=RESOLVE_D(DestroyDevice);
    VkQueue queue=VK_NULL_HANDLE;getQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;
    VkCommandPool pool=VK_NULL_HANDLE;r=createPool(device,&pci,nullptr,&pool);
    VkCommandBuffer cmd=VK_NULL_HANDLE;
    if(r==VK_SUCCESS){VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;r=allocCmd(device,&cai,&cmd);}
    if(r==VK_SUCCESS){VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};r=beginCmd(cmd,&bi);}
    if(r==VK_SUCCESS)r=endCmd(cmd);
    if(r==VK_SUCCESS){VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;r=submit(queue,1,&si,VK_NULL_HANDLE);}
    if(r==VK_SUCCESS)r=queueWait(queue);
    std::printf("queue=%p command_buffer=%p submit_result=0x%08x\n",(void*)queue,(void*)cmd,(unsigned)r);
    if(pool)destroyPool(device,pool,nullptr);destroyDevice(device,nullptr);destroyInstance(instance,nullptr);
    return r==VK_SUCCESS?0:5;
}
