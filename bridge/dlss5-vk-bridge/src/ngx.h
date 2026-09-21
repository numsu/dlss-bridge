// NGX declarations, inlined so the project has no dependency on NVIDIA's NGX SDK
// headers. OptiScaler uses MSVC while this bridge uses the MinGW target, and the
// two compilers order same-name virtual overloads differently. The Parameter
// declarations below are reversed within each overload group so their vtable
// slots match the MSVC object supplied by OptiScaler.

#pragma once

#include <vulkan/vulkan.h>

typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_SUCCESS = 1;
static const NVSDK_NGX_Result NGX_FAIL    = (NVSDK_NGX_Result)0xBAD00000;   // NVSDK_NGX_Result_Fail
static const NVSDK_NGX_Result NGX_FAIL_FEATURE_NOT_SUPPORTED
                                          = (NVSDK_NGX_Result)0xBAD00001;   // NVSDK_NGX_Result_FAIL_FeatureNotSupported

// Public NGX feature identifiers used at the feature-create boundary. Values
// match nvsdk_ngx_defs.h (NVSDK_NGX_Feature_SuperSampling/_FrameGeneration/
// _RayReconstruction). Keep SDK ABI declarations centralized here rather than
// scattering numeric IDs through the interception policy.
static const int NVSDK_NGX_FEATURE_SUPER_SAMPLING = 1;
static const int NVSDK_NGX_FEATURE_FRAME_GENERATION = 11;
static const int NVSDK_NGX_FEATURE_RAY_RECONSTRUCTION = 13;

struct NVSDK_NGX_Handle { unsigned int Id; };

struct ID3D11Resource;   // opaque, only used to keep the vtable shape identical
struct ID3D12Resource;   // opaque; the D3D12 session sets real pointers via <d3d12.h>

struct NVSDK_NGX_Parameter
{
    // OptiScaler is built with MSVC, which places same-name virtual overloads
    // in reverse declaration order. This bridge is built for MinGW, whose
    // vtable follows declaration order. Declare each overload group reversed
    // here so calls from the bridge land on the MSVC slots defined by NVIDIA's
    // public header.
    virtual void Set(const char *InName, void *InValue) = 0;
    virtual void Set(const char *InName, ID3D12Resource *InValue) = 0;
    virtual void Set(const char *InName, ID3D11Resource *InValue) = 0;
    virtual void Set(const char *InName, int InValue) = 0;
    virtual void Set(const char *InName, unsigned int InValue) = 0;
    virtual void Set(const char *InName, double InValue) = 0;
    virtual void Set(const char *InName, float InValue) = 0;
    virtual void Set(const char *InName, unsigned long long InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char *InName, void **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D12Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D11Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, double *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, float *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned long long *OutValue) const = 0;

    virtual void Reset() = 0;
};

// ---------------------------------------------------------------------------
// NGX Vulkan resource ABI
//
// The game hands DLSS its textures as NVSDK_NGX_Resource_VK, set into the
// parameter block through Set(key, void*). The layout below matches NVIDIA's
// nvsdk_ngx_vk.h so the pointers read back correctly with Get(key, void**).
// ---------------------------------------------------------------------------

typedef enum NVSDK_NGX_Resource_VK_Type
{
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW = 0,
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER    = 1,
} NVSDK_NGX_Resource_VK_Type;

typedef struct NVSDK_NGX_ImageViewInfo_VK
{
    VkImageView             ImageView;
    VkImage                 Image;
    VkImageSubresourceRange SubresourceRange;
    VkFormat                Format;
    unsigned int            Width;
    unsigned int            Height;
} NVSDK_NGX_ImageViewInfo_VK;

typedef struct NVSDK_NGX_BufferInfo_VK
{
    VkBuffer     Buffer;
    unsigned int SizeInBytes;
} NVSDK_NGX_BufferInfo_VK;

typedef struct NVSDK_NGX_Resource_VK
{
    union
    {
        NVSDK_NGX_ImageViewInfo_VK ImageViewInfo;
        NVSDK_NGX_BufferInfo_VK    BufferInfo;
    } Resource;
    NVSDK_NGX_Resource_VK_Type Type;
    bool                       ReadWrite;
} NVSDK_NGX_Resource_VK;

// ---------------------------------------------------------------------------
// Function typedefs
// ---------------------------------------------------------------------------

typedef void (*PFN_NVSDK_NGX_ProgressCallback)(float, bool *);

// -- Vulkan entry points, the ones this layer hooks in the game -------------
typedef NVSDK_NGX_Result (*PFN_NGX_VK_Create)(
    VkCommandBuffer, int /*feature*/, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_NGX_VK_Create1)(
    VkDevice, VkCommandBuffer, int /*feature*/, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_NGX_VK_Evaluate)(
    VkCommandBuffer, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
    PFN_NVSDK_NGX_ProgressCallback);

// Init variants carry the VkInstance / VkPhysicalDevice / VkDevice the game
// bound NGX to. Hooking them is how the bridge learns which Vulkan device is
// the render device without guessing from a command buffer.
typedef NVSDK_NGX_Result (*PFN_NGX_VK_Init)(
    unsigned long long /*appId*/, const wchar_t * /*dataPath*/,
    VkInstance, VkPhysicalDevice, VkDevice, int /*version*/, const void *);
typedef NVSDK_NGX_Result (*PFN_NGX_VK_Init_Ext2)(
    unsigned long long /*appId*/, const wchar_t * /*dataPath*/,
    VkInstance, VkPhysicalDevice, VkDevice,
    PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
    int /*version*/, const void *);
typedef NVSDK_NGX_Result (*PFN_NGX_VK_Init_ProjectID)(
    const char *, int, const char *, const wchar_t *,
    VkInstance, VkPhysicalDevice, VkDevice, int, const void *);

// -- D3D12 entry points, the private session the bridge drives --------------
// (declared with the real d3d12.h types in d3d12_session.inc, where <d3d12.h>
//  is included; forward-only here so ngx.h stays Vulkan-only.)
