// Dispatch tables shared by both hosts of the bridge core.
//
// The core (vk_interop.inc) reaches every Vulkan entry point through a
// DeviceDispatch the host fills in: the layer from the next layer's
// vkGetDeviceProcAddr at vkCreateDevice, the ReShade add-on from the loader's
// exported vkGetDeviceProcAddr on the device ReShade reports. The struct is
// the contract, so it lives here rather than in either host.

// Passed as the family index by a host that cannot learn it (the add-on host
// sees queues by handle and capability, never by index).
static const uint32_t kUnknownFamily = 0xFFFFFFFFu;

// ---- dispatch tables -------------------------------------------------------
struct InstanceDispatch
{
    VkInstance                                     instance;
    PFN_vkGetInstanceProcAddr                      GetInstanceProcAddr;
    PFN_vkDestroyInstance                          DestroyInstance;
    PFN_vkEnumeratePhysicalDevices                 EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceMemoryProperties        GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties   GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceProperties2             GetPhysicalDeviceProperties2;
    PFN_vkEnumerateDeviceExtensionProperties       EnumerateDeviceExtensionProperties;
};

struct DeviceDispatch
{
    VkDevice                          device;
    VkPhysicalDevice                  phys;
    PFN_vkGetDeviceProcAddr           GetDeviceProcAddr;
    PFN_vkDestroyDevice               DestroyDevice;
    PFN_vkGetDeviceQueue              GetDeviceQueue;
    PFN_vkGetDeviceQueue2             GetDeviceQueue2;
    PFN_vkQueueSubmit                 QueueSubmit;
    PFN_vkQueueSubmit2                QueueSubmit2;
    PFN_vkQueuePresentKHR              QueuePresentKHR;
    PFN_vkCreateImage                 CreateImage;
    PFN_vkDestroyImage                DestroyImage;
    PFN_vkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_vkBindImageMemory             BindImageMemory;
    PFN_vkAllocateMemory              AllocateMemory;
    PFN_vkFreeMemory                  FreeMemory;
    PFN_vkCreateBuffer                CreateBuffer;
    PFN_vkDestroyBuffer               DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory            BindBufferMemory;
    PFN_vkCreateCommandPool           CreateCommandPool;
    PFN_vkDestroyCommandPool          DestroyCommandPool;
    PFN_vkAllocateCommandBuffers      AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers          FreeCommandBuffers;
    PFN_vkBeginCommandBuffer          BeginCommandBuffer;
    PFN_vkEndCommandBuffer            EndCommandBuffer;
    PFN_vkResetCommandBuffer          ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier          CmdPipelineBarrier;
    PFN_vkCmdCopyImage                CmdCopyImage;
    PFN_vkCmdBlitImage                CmdBlitImage;
    PFN_vkCmdCopyImageToBuffer        CmdCopyImageToBuffer;
    PFN_vkCmdCopyBufferToImage        CmdCopyBufferToImage;
    PFN_vkCmdClearColorImage          CmdClearColorImage;
    PFN_vkCreateImageView             CreateImageView;
    PFN_vkDestroyImageView            DestroyImageView;
    PFN_vkCreateSampler               CreateSampler;
    PFN_vkDestroySampler              DestroySampler;
    PFN_vkCreateDescriptorSetLayout   CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout  DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout        CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout       DestroyPipelineLayout;
    PFN_vkCreateShaderModule          CreateShaderModule;
    PFN_vkDestroyShaderModule         DestroyShaderModule;
    PFN_vkCreateComputePipelines      CreateComputePipelines;
    PFN_vkDestroyPipeline             DestroyPipeline;
    PFN_vkCreateDescriptorPool        CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool       DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets      AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets        UpdateDescriptorSets;
    PFN_vkCmdBindPipeline             CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets       CmdBindDescriptorSets;
    PFN_vkCmdPushConstants            CmdPushConstants;
    PFN_vkCmdDispatch                 CmdDispatch;
    PFN_vkCmdExecuteCommands          CmdExecuteCommands;
    // windows.h renames CreateEvent to CreateEventW; the GD() loader below
    // renames its uses identically (same mechanism CreateSemaphore relies on),
    // so the name stays consistent everywhere without an #undef.
    PFN_vkCreateEvent                 CreateEvent;
    PFN_vkDestroyEvent                DestroyEvent;
    PFN_vkSetEvent                    SetEvent;
    PFN_vkResetEvent                  ResetEvent;
    PFN_vkGetEventStatus              GetEventStatus;
    PFN_vkCmdSetEvent                 CmdSetEvent;
    PFN_vkCmdWaitEvents               CmdWaitEvents;
    PFN_vkCreateSemaphore             CreateSemaphore;
    PFN_vkDestroySemaphore            DestroySemaphore;
    PFN_vkWaitSemaphores              WaitSemaphores;
    PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR;
    PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR;   // add-on host only
    PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT;
    PFN_vkCreateFence                 CreateFence;
    PFN_vkDestroyFence                DestroyFence;
    PFN_vkWaitForFences               WaitForFences;
    PFN_vkResetFences                 ResetFences;
    PFN_vkGetFenceStatus              GetFenceStatus;
    PFN_vkDeviceWaitIdle              DeviceWaitIdle;
};
