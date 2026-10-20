// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

// минимальные объявления ABI для динамической загрузки драйвера.
// реализаций и заголовков NVIDIA SDK здесь нет.
using NVSDK_NGX_Result=std::uint32_t;
using NVSDK_NGX_Version=std::uint32_t;
using NVSDK_NGX_EngineType=std::uint32_t;
using NVSDK_NGX_Feature=std::uint32_t;
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;
using PFN_NVSDK_NGX_ProgressCallback=void(*)(float,bool&);
inline constexpr NVSDK_NGX_Version NVSDK_NGX_Version_API=0x15;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success=1;
inline constexpr NVSDK_NGX_EngineType NVSDK_NGX_ENGINE_TYPE_CUSTOM=0;
inline constexpr NVSDK_NGX_Feature NVSDK_NGX_Feature_SuperSampling=1;
inline constexpr int NVSDK_NGX_PerfQuality_Value_MaxQuality=2;
inline constexpr int NVSDK_NGX_PerfQuality_Value_DLAA=5;
inline constexpr int NVSDK_NGX_DLSS_Feature_Flags_MVLowRes=2;
inline constexpr int NVSDK_NGX_DLSS_Feature_Flags_AutoExposure=64;
inline bool NVSDK_NGX_FAILED(NVSDK_NGX_Result value){return (value&0xfff00000u)==0xbad00000u;}
inline bool NVSDK_NGX_SUCCEED(NVSDK_NGX_Result value){return !NVSDK_NGX_FAILED(value);}
struct NVSDK_NGX_FeatureCommonInfo{
    struct {const wchar_t* const* Path;unsigned Length;} PathListInfo;
    void* InternalData;
    struct {void(*callback)(const char*,int,int);int level;bool disableOther;} LoggingInfo;
};
struct NVSDK_NGX_Resource_VK{
    union {
        struct {VkImageView ImageView;VkImage Image;VkImageSubresourceRange SubresourceRange;VkFormat Format;unsigned Width,Height;} ImageViewInfo;
        struct {VkBuffer Buffer;unsigned SizeInBytes;} BufferInfo;
    } Resource;
    std::uint32_t Type;
    bool ReadWrite;
};
static_assert(sizeof(NVSDK_NGX_Resource_VK)==56);
static_assert(sizeof(NVSDK_NGX_FeatureCommonInfo)==40);
inline constexpr auto NVSDK_NGX_Parameter_Color="Color";
inline constexpr auto NVSDK_NGX_Parameter_CreationNodeMask="CreationNodeMask";
inline constexpr auto NVSDK_NGX_Parameter_DLSS_Exposure_Scale="DLSS.Exposure.Scale";
inline constexpr auto NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags="DLSS.Feature.Create.Flags";
inline constexpr auto NVSDK_NGX_Parameter_DLSS_Pre_Exposure="DLSS.Pre.Exposure";
inline constexpr auto NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height="DLSS.Render.Subrect.Dimensions.Height";
inline constexpr auto NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width="DLSS.Render.Subrect.Dimensions.Width";
inline constexpr auto NVSDK_NGX_Parameter_Depth="Depth";
inline constexpr auto NVSDK_NGX_Parameter_FrameTimeDeltaInMsec="FrameTimeDeltaInMsec";
inline constexpr auto NVSDK_NGX_Parameter_Height="Height";
inline constexpr auto NVSDK_NGX_Parameter_Jitter_Offset_X="Jitter.Offset.X";
inline constexpr auto NVSDK_NGX_Parameter_Jitter_Offset_Y="Jitter.Offset.Y";
inline constexpr auto NVSDK_NGX_Parameter_MV_Scale_X="MV.Scale.X";
inline constexpr auto NVSDK_NGX_Parameter_MV_Scale_Y="MV.Scale.Y";
inline constexpr auto NVSDK_NGX_Parameter_MotionVectors="MotionVectors";
inline constexpr auto NVSDK_NGX_Parameter_OutHeight="OutHeight";
inline constexpr auto NVSDK_NGX_Parameter_OutWidth="OutWidth";
inline constexpr auto NVSDK_NGX_Parameter_Output="Output";
inline constexpr auto NVSDK_NGX_Parameter_PerfQualityValue="PerfQualityValue";
inline constexpr auto NVSDK_NGX_Parameter_Reset="Reset";
inline constexpr auto NVSDK_NGX_Parameter_VisibilityNodeMask="VisibilityNodeMask";
inline constexpr auto NVSDK_NGX_Parameter_Width="Width";
