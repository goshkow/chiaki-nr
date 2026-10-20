#include "nvidiavideo.h"
#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include <QCryptographicHash>
#include <QDebug>
#include <QMutex>
#include <windows.h>
#include <shaderc/shaderc.hpp>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>
#include <atomic>
#include <cmath>
#include <thread>
#include <chrono>
#include <QHash>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/ffx_upscale.h>
#include "vendor/NIS_Config.h"
#include "vendor/nis_shader_source.h"
extern "C" {
#include <libplacebo/shaders/custom.h>
}
#include <ffx_api/vk/ffx_api_vk.h>

namespace {
pl_vulkan ffxVulkan=nullptr;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ffxResolve(VkDevice device,const char* name){
    auto result=vkGetDeviceProcAddr(device,name);
    if(!result&&QString::fromLatin1(name).endsWith("KHR")){
        QByteArray core=QByteArray(name);core.chop(3);result=vkGetDeviceProcAddr(device,core.constData());
    }
    if(!result&&ffxVulkan)result=ffxVulkan->get_proc_addr(ffxVulkan->instance,name);
    if(!result&&!QString::fromLatin1(name).contains("DebugUtils"))qWarning()<<"FFX Vulkan missing proc"<<name;
    return result;
}

// NGX parameters are created by a Microsoft-ABI DLL. Calling overloaded
// virtual methods through MinGW's C++ ABI would use the wrong slots.
// These are the Microsoft x64 slots, validated by a set/get round trip.
template<typename T> void put(NVSDK_NGX_Parameter *p, int slot, const char *key, T value) {
    auto table=*reinterpret_cast<void***>(p);
    reinterpret_cast<void(*)(void*,const char*,T)>(table[slot])(p,key,value);
}
void ui(NVSDK_NGX_Parameter*p,const char*k,unsigned v){put(p,4,k,v);}
void si(NVSDK_NGX_Parameter*p,const char*k,int v){put(p,3,k,v);}
void fp(NVSDK_NGX_Parameter*p,const char*k,float v){put(p,6,k,v);}
void ptr(NVSDK_NGX_Parameter*p,const char*k,void*v){put(p,0,k,v);}
NVSDK_NGX_Result getUI(NVSDK_NGX_Parameter*p,const char*k,unsigned*v) {
    return reinterpret_cast<NVSDK_NGX_Result(*)(void*,const char*,unsigned*)>((*reinterpret_cast<void***>(p))[12])(p,k,v);
}
void check(VkResult result,const char *what){if(result!=VK_SUCCESS)throw std::runtime_error(what);}
struct Api {
    HMODULE module=nullptr;
    using Init=NVSDK_NGX_Result(*)(const char*,NVSDK_NGX_EngineType,const char*,const wchar_t*,VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
    Init init=nullptr;
    using InitLegacy=NVSDK_NGX_Result(*)(const char*,NVSDK_NGX_EngineType,const char*,const wchar_t*,VkInstance,VkPhysicalDevice,VkDevice,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
    InitLegacy legacyInit=nullptr;
    NVSDK_NGX_Result(*allocate)(NVSDK_NGX_Parameter**)=nullptr;
    NVSDK_NGX_Result(*destroy)(NVSDK_NGX_Parameter*)=nullptr;
    NVSDK_NGX_Result(*create)(VkCommandBuffer,NVSDK_NGX_Feature,const NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**)=nullptr;
    NVSDK_NGX_Result(*evaluate)(VkCommandBuffer,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback)=nullptr;
    NVSDK_NGX_Result(*release)(NVSDK_NGX_Handle*)=nullptr;
    NVSDK_NGX_Result(*shutdown)(VkDevice)=nullptr;
    bool initialized=false;
    bool load(const QString &path) {
        module=LoadLibraryExW(reinterpret_cast<const wchar_t*>(path.utf16()),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!module)return false;
#define LOAD(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(module,name)); if(!member)return false;
        init=reinterpret_cast<Init>(GetProcAddress(module,"NVSDK_NGX_VULKAN_Init_ProjectID_Ext"));
        if(!init)legacyInit=reinterpret_cast<InitLegacy>(GetProcAddress(module,"NVSDK_NGX_VULKAN_Init_ProjectID"));
        if(!init&&!legacyInit)return false;
        LOAD(allocate,"NVSDK_NGX_VULKAN_AllocateParameters")
        LOAD(destroy,"NVSDK_NGX_VULKAN_DestroyParameters")
        LOAD(create,"NVSDK_NGX_VULKAN_CreateFeature")
        LOAD(evaluate,"NVSDK_NGX_VULKAN_EvaluateFeature")
        LOAD(release,"NVSDK_NGX_VULKAN_ReleaseFeature")
        LOAD(shutdown,"NVSDK_NGX_VULKAN_Shutdown1")
#undef LOAD
        return true;
    }
};
bool hasRtxVulkan(){
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
    VkInstance instance{};if(vkCreateInstance(&info,nullptr,&instance)!=VK_SUCCESS)return false;
    uint32_t count=0;bool supported=false;
    if(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS&&count){
        std::vector<VkPhysicalDevice> devices(count);
        if(vkEnumeratePhysicalDevices(instance,&count,devices.data())==VK_SUCCESS)for(auto device:devices){VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(device,&p);if(p.vendorID==0x10de&&QString::fromUtf8(p.deviceName).contains("RTX",Qt::CaseInsensitive))supported=true;}
    }
    vkDestroyInstance(instance,nullptr);return supported;
}
QString corePath() {
    QDir store(QString::fromLocal8Bit(qgetenv("WINDIR"))+"/System32/DriverStore/FileRepository");
    for(const QString &name:store.entryList({"nv*"},QDir::Dirs|QDir::NoDotAndDotDot)) {
        QString path=store.filePath(name+"/nvngx.dll");
        if(QFile::exists(path))return path;
    }
    return {};
}
const char *guideShader=R"(
#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0,rgba16f) readonly uniform image2D currentColor;
layout(binding=1,rgba16f) readonly uniform image2D previousColor;
layout(binding=2,rg32f) writeonly uniform image2D motion;
layout(binding=3,r32f) writeonly uniform image2D depth;
layout(push_constant) uniform Push { int width; int height; int reset; } pc;
float luma(vec3 c){return dot(c,vec3(.2126,.7152,.0722));}
float sampleLuma(ivec2 p,bool old){p=clamp(p,ivec2(0),ivec2(pc.width-1,pc.height-1));return luma(old?imageLoad(previousColor,p).rgb:imageLoad(currentColor,p).rgb);}
void main(){
 ivec2 base=ivec2(gl_GlobalInvocationID.xy)*8;
 if(base.x>=pc.width||base.y>=pc.height)return;
 vec2 mv=vec2(0);float best=1e30;float runner=1e30;
 if(pc.reset==0){
  // Coarse-to-fine search handles camera motion beyond eight pixels without
  // increasing the number of color samples over the old fine-only search.
  for(int y=-32;y<=32;y+=8)for(int x=-32;x<=32;x+=8){
   float err=0;
   for(int j=-1;j<=1;j+=2)for(int i=-1;i<=1;i+=2){ivec2 p=base+ivec2(i,j)*4;err+=abs(sampleLuma(p,false)-sampleLuma(p+ivec2(x,y),true));}
   err+=.00005*float(x*x+y*y);
   if(err<best){best=err;mv=vec2(x,y);}
  }
  ivec2 coarse=ivec2(mv);best=1e30;runner=1e30;
  for(int y=-4;y<=4;++y)for(int x=-4;x<=4;++x){
   ivec2 offset=coarse+ivec2(x,y);
   float err=0;
   for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){ivec2 p=base+ivec2(i,j)*4;err+=abs(sampleLuma(p,false)-sampleLuma(p+offset,true));}
   err+=.00005*float(dot(vec2(offset),vec2(offset)));
   if(err<best){runner=best;best=err;mv=vec2(offset);}else runner=min(runner,err);
  }
  float zeroError=0;for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){ivec2 p=base+ivec2(i,j)*4;zeroError+=abs(sampleLuma(p,false)-sampleLuma(p,true));}
  if(best>=zeroError*.85||zeroError<.01)mv=vec2(0);
  // Refuse uncertain flow instead of feeding large arbitrary motion to DLSS.
  if(best>.6||(best>.1&&runner-best<.002))mv=vec2(0);
 }
 for(int j=0;j<8;++j)for(int i=0;i<8;++i){ivec2 p=base+ivec2(i,j);if(p.x<pc.width&&p.y<pc.height){imageStore(motion,p,vec4(mv,0,0));imageStore(depth,p,vec4(.5));}}
})";
const char *blendShader=R"(
#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0,rgba16f) readonly uniform image2D originalColor;
layout(binding=4,rgba16f) uniform image2D processedColor;
layout(binding=5,rgba16f) readonly uniform image2D spatialColor;
layout(push_constant) uniform Push { float strength; int hybrid; } pc;
vec4 at(ivec2 p){return imageLoad(originalColor,clamp(p,ivec2(0),imageSize(originalColor)-1));}
void main(){
 ivec2 p=ivec2(gl_GlobalInvocationID.xy), size=imageSize(processedColor);
 if(any(greaterThanEqual(p,size)))return;
 vec2 q=(vec2(p)+.5)*vec2(imageSize(originalColor))/vec2(size)-.5;
 ivec2 b=ivec2(floor(q)); vec2 f=fract(q);
 vec4 raw=mix(mix(at(b),at(b+ivec2(1,0)),f.x),mix(at(b+ivec2(0,1)),at(b+ivec2(1,1)),f.x),f.y);
 vec4 result=mix(raw,imageLoad(processedColor,p),pc.strength);
 // Reject color excursions absent in the current decoded frame instead of
 // boosting compression noise. The temporal SR still supplies the image.
 vec3 mn=vec3(1e30),mx=vec3(-1e30);
 for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){vec3 c=pc.hybrid!=0?imageLoad(spatialColor,clamp(p+ivec2(x,y),ivec2(0),imageSize(spatialColor)-1)).rgb:at(b+ivec2(x,y)).rgb;mn=min(mn,c);mx=max(mx,c);}
 if(any(isnan(result.rgb))||any(isinf(result.rgb)))result=raw;
 result.rgb=clamp(result.rgb,mn-.035,mx+.035);
 imageStore(processedColor,p,vec4(result.rgb,1));
})";

}

struct NvidiaVideo::Impl {
    mutable QRecursiveMutex mutex;
    mutable QMutex controlMutex;
    unsigned controlDirty=0;
    bool uiFsr=false;float uiFinalSharp=0;bool uiEnabled=false,uiNeural=false,uiFg=false,uiHybrid=false,uiBefore=false;
    int uiQuality=0,uiPreset=0,uiPasses=1,uiStyle=0;
    float uiBlend=.5f,uiScale=.5f,uiIntensity=1.f,uiNis=.25f;
    QString uiStatus=QStringLiteral("Выключено"),uiFgStatus=QStringLiteral("Выключено");double uiMs=0;
    float nisSharpness=.25f;
    VkQueryPool timingPool=VK_NULL_HANDLE;float timestampPeriod=0;QVariantMap timings,uiTimings;
    bool rtxCandidate=false,nrRuntimePresent=false,nrRejected=false;
    bool forceFgValidation=false;double estimatedCoreMs=0,estimatedFgMs=3.;uint64_t missedFgDeadlines=0;
    void stamp(unsigned index){if(timingPool)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timingPool,index);}
    void readTimings(){if(!timingPool)return;uint64_t ticks[8]{};if(vkGetQueryPoolResults(vk->device,timingPool,0,8,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)!=VK_SUCCESS)return;
        auto ms=[&](int a,int b){return double(ticks[b]-ticks[a])*timestampPeriod/1e6;};timings={{"motion",ms(0,1)},{"sr",ms(2,3)},{"sharpness",ms(3,4)},{"nr",ms(1,2)+ms(4,5)},{"fg",ms(5,6)}};estimatedCoreMs=ms(0,5);if(fg&&generatedValid)estimatedFgMs=ms(5,6);
    }

    void consumeControls(){
        QMutexLocker control(&controlMutex);unsigned flags=controlDirty;controlDirty=0;
        if(flags&512)fsr=uiFsr;if(flags&1024)finalSharp=0;
        if(flags&1)enabled=uiEnabled;if(flags&2)neural=uiNeural;if(flags&4)fg=uiFg;
        if(flags&8)hybrid=uiHybrid;if(flags&16)quality=uiQuality;if(flags&32)renderPreset=uiPreset;
        if(flags&64)neuralBlend=uiBlend;if(flags&128)nisSharpness=uiNis;
        if(flags&256){bool structural=nrScale!=uiScale||nrPasses!=uiPasses||nrStyle!=uiStyle||neuralBefore!=uiBefore;
            nrScale=uiScale;nrPasses=uiPasses;nrStyle=uiStyle;nrIntensity=uiIntensity;neuralBefore=uiBefore;if(structural)++nrRevision;}
        if(flags){failed=false;if(flags&(63|512))history=false;}
    }
    void publish(){QMutexLocker control(&controlMutex);uiStatus=status;uiFgStatus=fgStatus;uiMs=ms;uiTimings=timings;}

    bool fsr=false,activeFsr=false,activeHybrid=false;float finalSharp=0;bool enabled=false,neural=false,failed=false,history=false,hybrid=false;
    int quality=0,activeQuality=-1,renderPreset=0,activeRenderPreset=-1; bool fg=false,activeFg=false;
    QString fgStatus=QStringLiteral("Выключено");
    HMODULE ffxModule=nullptr; ffxContext ffx=nullptr,fsrContext=nullptr;
    PfnFfxCreateContext ffxCreate=nullptr; PfnFfxDestroyContext ffxDestroy=nullptr; PfnFfxConfigure ffxConfig=nullptr; PfnFfxDispatch ffxDispatchWork=nullptr;
    ffxCreateBackendVKDesc ffxBackend{}; ffxCreateContextDescFrameGeneration ffxDesc{};
    double phaseMs=8.333333;bool pendingReal=false,wakeForInput=false;float lastSharpness=-1;
    uint64_t fgFrame=0; QElapsedTimer presentAge; bool generatedValid=false; bool activeNeural=false,activeEnabled=false;
    float neuralBlend=.5f;bool neuralBefore=false;
    float nrScale=.5f,nrIntensity=1.f;int nrPasses=1,nrStyle=0;
    unsigned nrRevision=0,activeNrRevision=0,nrRebuilds=0;uint64_t nrEvaluations=0;bool nrHistory=false;
    HMODULE nrForwarder=nullptr;NVSDK_NGX_Parameter *nrCaps=nullptr;
    std::array<void*,2> nrFeatures{};
    using NrInit=int(*)(const wchar_t*,const wchar_t*,void*,void*,void*,int);
    using NrCreate=void*(*)(void*,void*,unsigned,unsigned,int,float,int,float,float,float,int,int);
    using NrEval=int(*)(void*,void*,void*,void*,void*,void*,void*,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,int,int,float,int,float,float,float,int,float,float);
    NrInit nrInit=nullptr;NrCreate nrCreate=nullptr;NrEval nrEval=nullptr;void(*nrRelease)(void*)=nullptr;
    VkPipeline nrApplyPipeline=VK_NULL_HANDLE;VkDescriptorSet nrDescriptors=VK_NULL_HANDLE;
    int nrWidth=0,nrHeight=0;
    QString status=QStringLiteral("Выключено"); double ms=0;
    pl_vulkan vk=nullptr;
    std::array<pl_tex,16> textures{}; // input, previous, motion, depth, output
    std::array<VkImageView,16> views{};
    Api core,nr; Api *api=nullptr;
    NVSDK_NGX_Parameter *parameters=nullptr; NVSDK_NGX_Handle *feature=nullptr;
    VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cmd=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE; VkSemaphore timeline=VK_NULL_HANDLE; uint64_t serial=0;
    VkDescriptorSetLayout setLayout=VK_NULL_HANDLE; VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;
    VkPipeline blendPipeline=VK_NULL_HANDLE,nisPipeline=VK_NULL_HANDLE,rcasPipeline=VK_NULL_HANDLE,fgGuardPipeline=VK_NULL_HANDLE;
    VkPipeline scaleGuidePipeline=VK_NULL_HANDLE,downsamplePipeline=VK_NULL_HANDLE;VkDescriptorSet scaleGuideDescriptors=VK_NULL_HANDLE,downsampleDescriptors=VK_NULL_HANDLE;
    int srWidth=0,srHeight=0,srOutWidth=0,srOutHeight=0;
    VkSampler nisSampler=VK_NULL_HANDLE;VkDescriptorSet fgGuardDescriptors=VK_NULL_HANDLE;
    uint64_t generatedPresents=0,realPresents=0,lastGeneratedPresent=0,lastRealPresent=0;bool fgHistory=false;
    VkPipeline pipeline=VK_NULL_HANDLE; VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
    VkDescriptorSet descriptors=VK_NULL_HANDLE; VkQueue queue=VK_NULL_HANDLE;
    int width=0,height=0,outWidth=0,outHeight=0;
    std::atomic<unsigned long long> sequence{0};
    unsigned long long completedSequence=0;
    unsigned long long processedFrames=0;
    void finish() {
        if(!vk)return;
        pl_gpu_finish(vk->gpu); if(nr.module) vkDeviceWaitIdle(vk->device);
        if(queue){
            vk->lock_queue(vk,vk->queue_graphics.index,0);
            vkQueueWaitIdle(queue);
            vk->unlock_queue(vk,vk->queue_graphics.index,0);
        }
    }
    void clear() {
        if(!vk)return;
        finish();
        for(auto &handle:nrFeatures){if(handle&&nrRelease)nrRelease(handle);handle=nullptr;}
        if(nrCaps&&core.destroy)core.destroy(nrCaps);nrCaps=nullptr;
        if(nrApplyPipeline)vkDestroyPipeline(vk->device,nrApplyPipeline,nullptr);nrApplyPipeline=VK_NULL_HANDLE;
        if(fsrContext&&ffxDestroy)ffxDestroy(&fsrContext,nullptr);fsrContext=nullptr;
        if(ffx&&ffxDestroy)ffxDestroy(&ffx,nullptr);ffx=nullptr;generatedValid=false;pendingReal=wakeForInput=false;
        if(feature&&api){qInfo()<<"release feature";api->release(feature);qInfo()<<"released feature";}feature=nullptr;
        if(parameters&&api){qInfo()<<"destroy parameters";api->destroy(parameters);qInfo()<<"destroyed parameters";}parameters=nullptr;
        for(auto &view:views){if(view)vkDestroyImageView(vk->device,view,nullptr);view=VK_NULL_HANDLE;}
        for(auto &tex:textures)pl_tex_destroy(vk->gpu,&tex);
        for(auto pipeline:{nisPipeline,rcasPipeline,fgGuardPipeline,scaleGuidePipeline,downsamplePipeline})if(pipeline)vkDestroyPipeline(vk->device,pipeline,nullptr);
        nisPipeline=rcasPipeline=fgGuardPipeline=scaleGuidePipeline=downsamplePipeline=VK_NULL_HANDLE;nrDescriptors=fgGuardDescriptors=VK_NULL_HANDLE;
        if(timingPool)vkDestroyQueryPool(vk->device,timingPool,nullptr);timingPool=VK_NULL_HANDLE;
        if(nisSampler)vkDestroySampler(vk->device,nisSampler,nullptr);nisSampler=VK_NULL_HANDLE;fgHistory=false;
        if(blendPipeline)vkDestroyPipeline(vk->device,blendPipeline,nullptr);blendPipeline=VK_NULL_HANDLE;
        if(pipeline)vkDestroyPipeline(vk->device,pipeline,nullptr);
        if(pipelineLayout)vkDestroyPipelineLayout(vk->device,pipelineLayout,nullptr);
        if(descriptorPool)vkDestroyDescriptorPool(vk->device,descriptorPool,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(vk->device,setLayout,nullptr);
        if(timeline)vkDestroySemaphore(vk->device,timeline,nullptr);
        if(fence)vkDestroyFence(vk->device,fence,nullptr);
        if(pool)vkDestroyCommandPool(vk->device,pool,nullptr);
        pipeline=VK_NULL_HANDLE;pipelineLayout=VK_NULL_HANDLE;descriptorPool=VK_NULL_HANDLE;setLayout=VK_NULL_HANDLE;
        timeline=VK_NULL_HANDLE;fence=VK_NULL_HANDLE;pool=VK_NULL_HANDLE;cmd=VK_NULL_HANDLE;
        width=height=outWidth=outHeight=0;history=false;
    }
    void begin(){check(vkResetCommandPool(vk->device,pool,0),"reset command pool");VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(cmd,&b),"begin command buffer");}
    void submit(const std::vector<uint64_t> &values={}) {
        check(vkEndCommandBuffer(cmd),"end command buffer");
        std::vector<VkSemaphore> sem(values.size(),timeline);
        std::vector<VkPipelineStageFlags> stages(values.size(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        // One timeline semaphore wait at its maximum value satisfies every hold.
        uint64_t waitValue=values.empty()?0:*std::max_element(values.begin(),values.end());
        VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};ti.waitSemaphoreValueCount=waitValue?1:0;ti.pWaitSemaphoreValues=&waitValue;
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.pNext=&ti;s.commandBufferCount=1;s.pCommandBuffers=&cmd;
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if(waitValue){s.waitSemaphoreCount=1;s.pWaitSemaphores=&timeline;s.pWaitDstStageMask=&stage;}
        vkResetFences(vk->device,1,&fence);
        vk->lock_queue(vk,vk->queue_graphics.index,0);
        VkResult result=vkQueueSubmit(queue,1,&s,fence);
        vk->unlock_queue(vk,vk->queue_graphics.index,0);
        check(result,"submit NVIDIA work");
        check(vkWaitForFences(vk->device,1,&fence,VK_TRUE,3000000000ULL),"NVIDIA GPU timeout");
    }
    NVSDK_NGX_Resource_VK resource(int i) {
        VkFormat format;VkImage image=pl_vulkan_unwrap(vk->gpu,textures[i],&format,nullptr);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        return NVSDK_NGX_Create_ImageView_Resource_VK(views[i],image,range,format,textures[i]->params.w,textures[i]->params.h,i==4||i>=7);
    }
    FfxApiResource ffxResource(int i) {
        VkFormat fmt;VkImage image=pl_vulkan_unwrap(vk->gpu,textures[i],&fmt,nullptr);
        FfxApiResourceDescription desc{};desc.type=FFX_API_RESOURCE_TYPE_TEXTURE2D;
        desc.format=ffxApiGetSurfaceFormatVK(fmt);desc.width=textures[i]->params.w;desc.height=textures[i]->params.h;desc.depth=1;desc.mipCount=1;
        desc.usage=FFX_API_RESOURCE_USAGE_UAV;
        return ffxApiGetResourceVK(reinterpret_cast<void*>(image),desc,FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void recordPresentation(bool generated){if(!fg)return;if(generated){if(lastGeneratedPresent!=fgFrame){++generatedPresents;lastGeneratedPresent=fgFrame;}}else if(lastRealPresent!=fgFrame){++realPresents;lastRealPresent=fgFrame;}fgStatus=QStringLiteral("Видео OF · обработано: %1 · показано фаз: исходные %2 / промежуточные %3 · буфер %4 мс").arg(fgFrame).arg(realPresents).arg(generatedPresents).arg(phaseMs,0,'f',1);publish();}
    bool cachedPresent(pl_renderer renderer,const pl_frame *target,const pl_render_params *params){
        const bool generated=fg&&generatedValid&&pendingReal&&presentAge.isValid()&&presentAge.nsecsElapsed()<phaseMs*1e6;
        if(pendingReal&&!generated){pendingReal=false;wakeForInput=true;}
        pl_frame cached{};cached.num_planes=1;cached.planes[0].components=4;for(int i=0;i<4;++i)cached.planes[0].component_mapping[i]=i;
        cached.planes[0].texture=textures[generated?5:4];cached.repr=pl_color_repr_rgb;cached.repr.alpha=PL_ALPHA_NONE;cached.color=pl_color_space_srgb;cached.crop={0,0,float(outWidth),float(outHeight)};recordPresentation(generated);return pl_render_image(renderer,&cached,target,params);
    }
    void imageBarrier(){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);}
    void copyFull(int source,int target){imageBarrier();VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;copy.extent={unsigned(textures[source]->params.w),unsigned(textures[source]->params.h),1};VkFormat fmt;vkCmdCopyImage(cmd,pl_vulkan_unwrap(vk->gpu,textures[source],&fmt,nullptr),VK_IMAGE_LAYOUT_GENERAL,pl_vulkan_unwrap(vk->gpu,textures[target],&fmt,nullptr),VK_IMAGE_LAYOUT_GENERAL,1,&copy);imageBarrier();}
    void sharpen(float rcas,float overrideStrength=-1,bool forceRcas=false){bool useNis=enabled&&!fsr&&!forceRcas;float strength=overrideStrength>=0?overrideStrength:enabled?nisSharpness:rcas;if(strength<=0)return;copyFull(4,11);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,useNis?nisPipeline:rcasPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptors,0,nullptr);
        if(useNis){NISConfig config{};NVSharpenUpdateConfig(config,strength,0,0,outWidth,outHeight,outWidth,outHeight,0,0);vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,112,&config);vkCmdDispatch(cmd,(outWidth+31)/32,(outHeight+31)/32,1);}else{vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&strength);vkCmdDispatch(cmd,(outWidth+7)/8,(outHeight+7)/8,1);}imageBarrier();
    }
    void generate() {
        ++fgFrame;
        generatedValid=history&&fgHistory;fgHistory=true;
        if(generatedValid){imageBarrier();vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,fgGuardPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&fgGuardDescriptors,0,nullptr);vkCmdDispatch(cmd,(outWidth+7)/8,(outHeight+7)/8,1);imageBarrier();}
    }

    bool init(int w,int h,int ow,int oh) {
        clear();width=w;height=h;outWidth=ow;outHeight=oh;
        srWidth=hybrid?ow:w;srHeight=hybrid?oh:h;
        double ss=hybrid?std::max(1.5,2160./oh):1.;
        srOutWidth=hybrid?int(ow*ss+.5):ow;srOutHeight=hybrid?int(oh*ss+.5):oh;
        if(neural||enabled){
        if(!core.module&&!core.load(corePath()))throw std::runtime_error("NVIDIA NGX core unavailable");
        api=&core;
        if(!api->initialized){
            auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk->get_proc_addr(vk->instance,"vkGetDeviceProcAddr"));
            QString logPath=QDir::tempPath()+"/chiaki-ngx";QDir().mkpath(logPath);
            QString runtimeDir=QCoreApplication::applicationDirPath();const wchar_t *path=reinterpret_cast<const wchar_t*>(runtimeDir.utf16());
            NVSDK_NGX_FeatureCommonInfo info{};info.PathListInfo.Path= &path;info.PathListInfo.Length=1;
            auto r=api->init
                ?api->init("54c83193-059a-43e5-94ee-987a45ed3c63",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"Chiaki-NVIDIA-1",reinterpret_cast<const wchar_t*>(logPath.utf16()),vk->instance,vk->phys_device,vk->device,vk->get_proc_addr,gdpa,NVSDK_NGX_Version_API,&info)
                :api->legacyInit("54c83193-059a-43e5-94ee-987a45ed3c63",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"Chiaki-NVIDIA-1",reinterpret_cast<const wchar_t*>(logPath.utf16()),vk->instance,vk->phys_device,vk->device,NVSDK_NGX_Version_API,&info);
            qInfo()<<"Chiaki NVIDIA NGX init"<<Qt::hex<<unsigned(r)<<"neural route"<<neural;
            if(NVSDK_NGX_FAILED(r))throw std::runtime_error("NGX initialization failed");
            api->initialized=true;
        }
        if(NVSDK_NGX_FAILED(api->allocate(&parameters)))throw std::runtime_error("NGX parameter allocation failed");
        ui(parameters,"Chiaki.AbiProbe",12345);unsigned probe=0;
        if(NVSDK_NGX_FAILED(getUI(parameters,"Chiaki.AbiProbe",&probe))||probe!=12345)throw std::runtime_error("NGX parameter ABI check failed");
        }
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=vk->queue_graphics.index;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(vk->device,&pi,nullptr,&pool),"command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
        check(vkAllocateCommandBuffers(vk->device,&ai,&cmd),"command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(vk->device,&fi,nullptr,&fence),"fence");
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};sc.pNext=&type;check(vkCreateSemaphore(vk->device,&sc,nullptr,&timeline),"timeline");serial=0;
        vkGetDeviceQueue(vk->device,vk->queue_graphics.index,0,&queue);
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(vk->phys_device,&properties);timestampPeriod=properties.limits.timestampPeriod;
        if(properties.limits.timestampComputeAndGraphics&&timestampPeriod>0){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=8;check(vkCreateQueryPool(vk->device,&qi,nullptr,&timingPool),"video timestamp queries");}

        nrWidth=neural?std::max(1,int((neuralBefore?w:ow)*nrScale+.5f)):1;
        nrHeight=neural?std::max(1,int((neuralBefore?h:oh)*nrScale+.5f)):1;
        const char *formats[]={"rgba16f","rgba16f","rg32f","r32f","rgba16f","rgba16f","rgba16f","rgba16f","rgba16f","rgba16f","rg32f","rgba16f","rgba16f","rg32f","r32f","rgba16f"};
        for(int i=0;i<16;++i){
            pl_tex_params tp{};tp.w=i==15?srOutWidth:i==13||i==14?srWidth:i>=7&&i<=9?nrWidth:i==10?w:i>=4?ow:w;tp.h=i==15?srOutHeight:i==13||i==14?srHeight:i>=7&&i<=9?nrHeight:i==10?h:i>=4?oh:h;tp.format=pl_find_named_fmt(vk->gpu,formats[i]);
            tp.host_readable=QCoreApplication::arguments().contains("--nvidia-self-test")&&i>=4;tp.sampleable=true;tp.renderable=i==0||i==4||i==6;tp.storable=true;tp.blit_src=true;tp.blit_dst=true;
            textures[i]=pl_tex_create(vk->gpu,&tp);if(!textures[i])throw std::runtime_error("GPU image allocation failed");
            VkFormat format;VkImage image=pl_vulkan_unwrap(vk->gpu,textures[i],&format,nullptr);
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            check(vkCreateImageView(vk->device,&vi,nullptr,&views[i]),"image view");
        }
        std::array<VkDescriptorSetLayoutBinding,8> bindings{};
        for(int i=0;i<6;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        bindings[6]={6,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};bindings[7]={7,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=8;li.pBindings=bindings.data();check(vkCreateDescriptorSetLayout(vk->device,&li,nullptr,&setLayout),"descriptor layout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,112};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;check(vkCreatePipelineLayout(vk->device,&pli,nullptr,&pipelineLayout),"pipeline layout");
        shaderc::Compiler compiler;auto spirv=compiler.CompileGlslToSpv(guideShader,shaderc_compute_shader,"chiaki-motion.comp");
        if(spirv.GetCompilationStatus()!=shaderc_compilation_status_success)throw std::runtime_error(spirv.GetErrorMessage());
        std::vector<uint32_t> code(spirv.cbegin(),spirv.cend());VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=code.size()*4;sm.pCode=code.data();VkShaderModule module;check(vkCreateShaderModule(vk->device,&sm,nullptr,&module),"shader module");
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.layout=pipelineLayout;cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=module;cp.stage.pName="main";
        VkResult created=vkCreateComputePipelines(vk->device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline);vkDestroyShaderModule(vk->device,module,nullptr);check(created,"guide pipeline");
        auto blendSpirv=compiler.CompileGlslToSpv(blendShader,shaderc_compute_shader,"chiaki-nr-blend.comp");
        if(blendSpirv.GetCompilationStatus()!=shaderc_compilation_status_success)throw std::runtime_error(blendSpirv.GetErrorMessage());
        code.assign(blendSpirv.cbegin(),blendSpirv.cend());sm.codeSize=code.size()*4;sm.pCode=code.data();
        check(vkCreateShaderModule(vk->device,&sm,nullptr,&module),"blend shader");cp.stage.module=module;
        created=vkCreateComputePipelines(vk->device,VK_NULL_HANDLE,1,&cp,nullptr,&blendPipeline);vkDestroyShaderModule(vk->device,module,nullptr);check(created,"blend pipeline");

        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,30},{VK_DESCRIPTOR_TYPE_SAMPLER,5},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,5}};VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=5;dpi.poolSizeCount=3;dpi.pPoolSizes=sizes;check(vkCreateDescriptorPool(vk->device,&dpi,nullptr,&descriptorPool),"descriptor pool");
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=descriptorPool;dai.descriptorSetCount=1;dai.pSetLayouts=&setLayout;check(vkAllocateDescriptorSets(vk->device,&dai,&descriptors),"descriptor set");
        std::array<VkDescriptorImageInfo,6> imageInfos{};std::array<VkWriteDescriptorSet,6> writes{};
        for(int i=0;i<6;++i){imageInfos[i].imageView=views[i==5?6:i];imageInfos[i].imageLayout=VK_IMAGE_LAYOUT_GENERAL;writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=descriptors;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&imageInfos[i];}
        vkUpdateDescriptorSets(vk->device,6,writes.data(),0,nullptr);
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};samplerInfo.magFilter=samplerInfo.minFilter=VK_FILTER_LINEAR;samplerInfo.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(vk->device,&samplerInfo,nullptr,&nisSampler),"NIS sampler");
        VkDescriptorImageInfo extraImages[]={{nisSampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED},{VK_NULL_HANDLE,views[11],VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet extraWrites[2]{};for(int i=0;i<2;++i){extraWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};extraWrites[i].dstSet=descriptors;extraWrites[i].dstBinding=6+i;extraWrites[i].descriptorCount=1;extraWrites[i].descriptorType=i?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLER;extraWrites[i].pImageInfo=&extraImages[i];}vkUpdateDescriptorSets(vk->device,2,extraWrites,0,nullptr);
        auto compilePipeline=[&](const char *source,const char *name,VkPipeline &pipeline){static QHash<QString,std::vector<uint32_t>> cache;if(!cache.contains(QString::fromUtf8(name))){auto spv=compiler.CompileGlslToSpv(source,shaderc_compute_shader,name);if(spv.GetCompilationStatus()!=shaderc_compilation_status_success)throw std::runtime_error(spv.GetErrorMessage());cache.insert(QString::fromUtf8(name),std::vector<uint32_t>(spv.cbegin(),spv.cend()));}code=cache.value(QString::fromUtf8(name));sm.codeSize=code.size()*4;sm.pCode=code.data();check(vkCreateShaderModule(vk->device,&sm,nullptr,&module),name);cp.stage.module=module;auto rc=vkCreateComputePipelines(vk->device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline);vkDestroyShaderModule(vk->device,module,nullptr);check(rc,name);};
        compilePipeline(nisShader,"NVIDIA-NIS-NVSharpen.comp",nisPipeline);
        const char *rcasCompute=R"RCASGPU(
#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=6) uniform sampler linearSampler;layout(binding=7) uniform texture2D source;
layout(binding=4,rgba16f) writeonly uniform image2D outputColor;
layout(push_constant) uniform Push{float strength;}pc;
vec3 at(ivec2 p){return texelFetch(sampler2D(source,linearSampler),clamp(p,ivec2(0),textureSize(sampler2D(source,linearSampler),0)-1),0).rgb;}
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(p,imageSize(outputColor))))return;vec3 e=at(p),b=at(p+ivec2(0,-1)),d=at(p+ivec2(-1,0)),f=at(p+ivec2(1,0)),h=at(p+ivec2(0,1));vec3 mn=min(min(b,d),min(f,h)),mx=max(max(b,d),max(f,h));vec3 hitMin=min(mn,e)/max(4.*mx,vec3(.000001));vec3 hitMax=(1.-max(mx,e))/min(4.*mn-4.,vec3(-.000001));vec3 lobes=max(vec3(-.1875),min(max(-hitMin,hitMax),vec3(0)));float contrast=max(max(mx.r-mn.r,mx.g-mn.g),mx.b-mn.b);vec3 lumaWeights=vec3(.2126,.7152,.0722);float bL=dot(b,lumaWeights),dL=dot(d,lumaWeights),eL=dot(e,lumaWeights),fL=dot(f,lumaWeights),hL=dot(h,lumaWeights);float range=max(max(max(bL,dL),max(fL,hL)),eL)-min(min(min(bL,dL),min(fL,hL)),eL);float noise=1.-.5*clamp(abs(.25*(bL+dL+fL+hL)-eL)/max(range,.000001),0.,1.);float lobe=max(max(lobes.r,lobes.g),lobes.b)*pc.strength*noise;imageStore(outputColor,p,vec4(clamp((lobe*(b+d+f+h)+e)/(4.*lobe+1.),0.,1.),1));}
)RCASGPU";
        compilePipeline(rcasCompute,"video-RCAS-noise-guard.comp",rcasPipeline);
        // Video interpolation uses decoded-image flow, no invented game depth.
        // This is a custom optical-flow backend, not AMD FSR or NVIDIA DLSSG.
        const char *guardShader=R"FGGUARD(
#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0,rgba16f) readonly uniform image2D previousColor;
layout(binding=1,rgba16f) readonly uniform image2D currentColor;
layout(binding=2,rg32f) readonly uniform image2D rawMotion;
layout(binding=4,rgba16f) writeonly uniform image2D generated;
vec3 at(ivec2 p,bool old){p=clamp(p,ivec2(0),imageSize(currentColor)-1);return old?imageLoad(previousColor,p).rgb:imageLoad(currentColor,p).rgb;}
vec3 sampleFull(vec2 q,bool old){ivec2 b=ivec2(floor(q));vec2 f=fract(q);return mix(mix(at(b,old),at(b+ivec2(1,0),old),f.x),mix(at(b+ivec2(0,1),old),at(b+ivec2(1,1),old),f.x),f.y);}
vec2 flowTap(ivec2 b){return imageLoad(rawMotion,clamp(b*8+ivec2(4),ivec2(0),imageSize(rawMotion)-1)).xy;}
vec2 flow(vec2 q){vec2 g=(q-4.)/8.;ivec2 b=ivec2(floor(g));vec2 f=fract(g);return mix(mix(flowTap(b),flowTap(b+ivec2(1,0)),f.x),mix(flowTap(b+ivec2(0,1)),flowTap(b+ivec2(1,1)),f.x),f.y);}
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy),size=imageSize(generated);if(any(greaterThanEqual(p,size)))return;
 vec2 scale=vec2(size)/vec2(imageSize(rawMotion)),q=(vec2(p)+.5)/scale-.5;
 vec2 mv=flow(q)*scale;
 // Inverse midpoint mapping: current->previous motion has the opposite
 // sign to the forward displacement. Refine once at the current endpoint.
 mv=flow(q-.5*mv/scale)*scale;
 vec3 current=sampleFull(vec2(p)-.5*mv,false),previous=sampleFull(vec2(p)+.5*mv,true);
 float disagreement=max(max(abs(current.r-previous.r),abs(current.g-previous.g)),abs(current.b-previous.b));
 // Reject occlusions and cuts instead of injecting speckle or double edges.
 vec3 value=disagreement<.12?mix(previous,current,.5):at(p,false);
 if(any(isnan(value))||any(isinf(value)))value=at(p,false);
 imageStore(generated,p,vec4(clamp(value,0.,1.),1));}
)FGGUARD";
        compilePipeline(guardShader,"video-optical-flow-midpoint.comp",fgGuardPipeline);
        check(vkAllocateDescriptorSets(vk->device,&dai,&fgGuardDescriptors),"video FG descriptors");
        VkDescriptorImageInfo guardImages[4]{};VkWriteDescriptorSet guardWrites[4]{};int guardIndices[]={12,4,2,5};int guardBindings[]={0,1,2,4};for(int i=0;i<4;++i){guardImages[i]={VK_NULL_HANDLE,views[guardIndices[i]],VK_IMAGE_LAYOUT_GENERAL};guardWrites[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};guardWrites[i].dstSet=fgGuardDescriptors;guardWrites[i].dstBinding=guardBindings[i];guardWrites[i].descriptorCount=1;guardWrites[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;guardWrites[i].pImageInfo=&guardImages[i];}vkUpdateDescriptorSets(vk->device,4,guardWrites,0,nullptr);

        const char *scaleShader=R"SCALE(
#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0,rg32f) readonly uniform image2D rawMotion;
layout(binding=2,rg32f) writeonly uniform image2D scaledMotion;
layout(binding=3,r32f) writeonly uniform image2D scaledDepth;
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy),size=imageSize(scaledMotion);if(any(greaterThanEqual(p,size)))return;ivec2 q=clamp(ivec2((vec2(p)+.5)*vec2(imageSize(rawMotion))/vec2(size)),ivec2(0),imageSize(rawMotion)-1);vec2 mv=imageLoad(rawMotion,q).xy*vec2(size)/vec2(imageSize(rawMotion));imageStore(scaledMotion,p,vec4(mv,0,0));imageStore(scaledDepth,p,vec4(.5));}
)SCALE";
        const char *downShader=R"DOWN(
#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0,rgba16f) readonly uniform image2D largeColor;
layout(binding=4,rgba16f) writeonly uniform image2D finalColor;
float sinc(float v){v=abs(v);return v<.0001?1.:sin(3.14159265*v)/(3.14159265*v);}
float kernel(float v){return abs(v)<2.?sinc(v)*sinc(v*.5):0.;}
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy),dst=imageSize(finalColor),src=imageSize(largeColor);if(any(greaterThanEqual(p,dst)))return;vec2 scale=vec2(src)/vec2(dst),q=(vec2(p)+.5)*scale-.5;ivec2 b=ivec2(floor(q));vec3 sum=vec3(0);float weight=0;for(int y=-3;y<=3;++y)for(int x=-3;x<=3;++x){ivec2 n=b+ivec2(x,y);vec2 delta=(vec2(n)-q)/scale;float k=kernel(delta.x)*kernel(delta.y);sum+=k*imageLoad(largeColor,clamp(n,ivec2(0),src-1)).rgb;weight+=k;}imageStore(finalColor,p,vec4(clamp(sum/max(weight,.0001),0.,1.),1));}
)DOWN";
        compilePipeline(scaleShader,"video-motion-upscale.comp",scaleGuidePipeline);
        compilePipeline(downShader,"video-4K-Lanczos-downsample.comp",downsamplePipeline);
        auto descriptor=[&](VkDescriptorSet &set,std::initializer_list<std::pair<int,int>> mapping){check(vkAllocateDescriptorSets(vk->device,&dai,&set),"stage descriptors");for(auto [binding,index]:mapping){VkDescriptorImageInfo image{VK_NULL_HANDLE,views[index],VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=binding;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&image;vkUpdateDescriptorSets(vk->device,1,&write,0,nullptr);}};
        descriptor(scaleGuideDescriptors,{{0,2},{2,13},{3,14}});
        descriptor(downsampleDescriptors,{{0,15},{4,4}});
        if(parameters){
        ui(parameters,NVSDK_NGX_Parameter_Width,srWidth);ui(parameters,NVSDK_NGX_Parameter_Height,srHeight);ui(parameters,NVSDK_NGX_Parameter_OutWidth,srOutWidth);ui(parameters,NVSDK_NGX_Parameter_OutHeight,srOutHeight);
        // A decoded video has a fixed native resolution, not a game render
        // percentage. Quality is an internal hint; the input stays w by h.
        int q=enabled&&srOutWidth>srWidth?NVSDK_NGX_PerfQuality_Value_MaxQuality:NVSDK_NGX_PerfQuality_Value_DLAA;
        si(parameters,NVSDK_NGX_Parameter_PerfQualityValue,q);
        si(parameters,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
        ui(parameters,NVSDK_NGX_Parameter_CreationNodeMask,1);ui(parameters,NVSDK_NGX_Parameter_VisibilityNodeMask,1);
        const char *hints[]={"DLSS.Hint.Render.Preset.DLAA","DLSS.Hint.Render.Preset.Quality","DLSS.Hint.Render.Preset.Balanced","DLSS.Hint.Render.Preset.Performance"};
        for(auto hint:hints)ui(parameters,hint,unsigned(renderPreset));
        }
        if(enabled&&!fsr&&srOutWidth>srWidth){
            begin();auto result=api->create(cmd,NVSDK_NGX_Feature_SuperSampling,parameters,&feature);
            qInfo()<<"Chiaki DLSS create"<<Qt::hex<<unsigned(result)<<w<<h<<ow<<oh;
            if(NVSDK_NGX_FAILED(result))throw std::runtime_error("DLSS feature creation failed");
            submit();
        }
        activeFsr=fsr;activeHybrid=hybrid;activeEnabled=enabled;activeQuality=quality;activeNeural=neural;activeFg=fg;activeRenderPreset=renderPreset;
        if(fsr){
            if(!ffxModule){
                QString path=QCoreApplication::applicationDirPath()+"/amd_fidelityfx_vk.dll";
                ffxModule=LoadLibraryExW(reinterpret_cast<const wchar_t*>(path.utf16()),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
                if(!ffxModule)throw std::runtime_error("AMD FidelityFX Vulkan library unavailable");
                ffxCreate=reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(ffxModule,"ffxCreateContext"));
                ffxDestroy=reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(ffxModule,"ffxDestroyContext"));
                ffxConfig=reinterpret_cast<PfnFfxConfigure>(GetProcAddress(ffxModule,"ffxConfigure"));
                ffxDispatchWork=reinterpret_cast<PfnFfxDispatch>(GetProcAddress(ffxModule,"ffxDispatch"));
                if(!ffxCreate||!ffxDestroy||!ffxConfig||!ffxDispatchWork)throw std::runtime_error("AMD FidelityFX exports unavailable");
            }
            ffxBackend={};ffxBackend.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
            ffxBackend.vkDevice=vk->device;ffxBackend.vkPhysicalDevice=vk->phys_device;
            ffxVulkan=vk;ffxBackend.vkDeviceProcAddr=ffxResolve;
            if(fsr){ffxCreateContextDescUpscale up{};up.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;up.header.pNext=&ffxBackend.header;up.flags=FFX_UPSCALE_ENABLE_AUTO_EXPOSURE|FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;up.maxRenderSize={unsigned(srWidth),unsigned(srHeight)};up.maxUpscaleSize={unsigned(srOutWidth),unsigned(srOutHeight)};auto rc=ffxCreate(&fsrContext,&up.header,nullptr);qInfo()<<"FFX upscale context"<<rc;if(rc)throw std::runtime_error("FSR upscaler context creation failed");}
            if(fg){
            ffxDesc={};ffxDesc.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;ffxDesc.header.pNext=&ffxBackend.header;
            ffxDesc.displaySize={unsigned(ow),unsigned(oh)};ffxDesc.maxRenderSize={unsigned(w),unsigned(h)};
            ffxDesc.backBufferFormat=FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            auto rc=ffxCreate(&ffx,&ffxDesc.header,nullptr);
            qInfo()<<"FFX FG context"<<rc;
            if(rc)throw std::runtime_error("FSR Frame Generation context creation failed");
            fgFrame=0;fgStatus=QStringLiteral("FSR 3.1 · контекст создан, ожидание кадров");
            }
        }
        if(neural)createNeural();
        activeNrRevision=nrRevision;
        return true;
    }
    void createNeural(){
        ++nrRebuilds;nrHistory=false;
        shaderc::Compiler compiler;std::vector<uint32_t> code;VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};VkShaderModule module{};VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.layout=pipelineLayout;cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.pName="main";VkResult created;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=descriptorPool;dai.descriptorSetCount=1;dai.pSetLayouts=&setLayout;
        std::array<VkDescriptorImageInfo,3> imageInfos{};std::array<VkWriteDescriptorSet,3> writes{};
            QString dir=QCoreApplication::applicationDirPath();
            if(!nrForwarder){
                QFile runtime(dir+"/nvngx_dlssnr.dll");
                if(!runtime.open(QIODevice::ReadOnly)||QCryptographicHash::hash(runtime.readAll(),QCryptographicHash::Sha256).toHex().toUpper()!="E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A")throw std::runtime_error("NR runtime SHA256 mismatch");
                nrForwarder=LoadLibraryExW(reinterpret_cast<const wchar_t*>((dir+"/chiaki_nvngx.dll").utf16()),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
                if(!nrForwarder)throw std::runtime_error("native NR forwarder unavailable");
                nrInit=reinterpret_cast<NrInit>(GetProcAddress(nrForwarder,"dlssnr_vk_init"));
                nrCreate=reinterpret_cast<NrCreate>(GetProcAddress(nrForwarder,"dlssnr_vk_create"));
                nrEval=reinterpret_cast<NrEval>(GetProcAddress(nrForwarder,"dlssnr_vk_evaluate_v2"));
                nrRelease=reinterpret_cast<void(*)(void*)>(GetProcAddress(nrForwarder,"dlssnr_vk_release"));
                auto floatSlot=reinterpret_cast<void(*)(int)>(GetProcAddress(nrForwarder,"dlssnr_call_set_float_slot"));
                if(!nrInit||!nrCreate||!nrEval||!nrRelease||!floatSlot)throw std::runtime_error("native NR entry points missing");
                floatSlot(6);
                if(nrInit(reinterpret_cast<const wchar_t*>((dir+"/nvngx_dlssnr.dll").utf16()),reinterpret_cast<const wchar_t*>(dir.utf16()),vk->instance,vk->phys_device,vk->device,0x15)!=1)throw std::runtime_error("native NR initialization failed");
            }
            auto getCap=reinterpret_cast<NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter**)>(GetProcAddress(core.module,"NVSDK_NGX_VULKAN_GetCapabilityParameters"));
            if(!getCap||NVSDK_NGX_FAILED(getCap(&nrCaps)))throw std::runtime_error("native NR capability parameters unavailable");
            begin();
            for(int pass=0;pass<nrPasses;++pass){
                nrFeatures[pass]=nrCreate(cmd,nrCaps,nrWidth,nrHeight,0,1.f,nrStyle,1.f,1.f,-1.f,1,0);
                if(!nrFeatures[pass])throw std::runtime_error("native NR feature 18 creation failed");
            }
            submit();
            if(!nrApplyPipeline){
            const char *nrShader=R"(
#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0,rgba16f) uniform image2D fullColor;
layout(binding=1,rgba16f) readonly uniform image2D workingOriginal;
layout(binding=2,rgba16f) readonly uniform image2D workingResult;
layout(push_constant) uniform Push {float strength;} pc;
vec4 tap(ivec2 p,bool result){p=clamp(p,ivec2(0),imageSize(workingOriginal)-1);return result?imageLoad(workingResult,p):imageLoad(workingOriginal,p);}
vec4 sampleSmall(vec2 q,bool result){ivec2 b=ivec2(floor(q));vec2 f=fract(q);return mix(mix(tap(b,result),tap(b+ivec2(1,0),result),f.x),mix(tap(b+ivec2(0,1),result),tap(b+ivec2(1,1),result),f.x),f.y);}
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(p,imageSize(fullColor))))return;
 vec2 q=(vec2(p)+.5)*vec2(imageSize(workingOriginal))/vec2(imageSize(fullColor))-.5;
 vec3 original=imageLoad(fullColor,p).rgb;
 // Apply a smoothed signed correction, retaining the full-resolution base.
 // Never replace that base with an enlarged low-resolution model image.
 vec3 delta=vec3(0);float weight=0;
 for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){float w=(x==0?2.:1.)*(y==0?2.:1.);vec2 at=q+vec2(x,y);delta+=w*(sampleSmall(at,true).rgb-sampleSmall(at,false).rgb);weight+=w;}
 delta/=weight;
 vec3 smallOriginal=sampleSmall(q,false).rgb,contrast=vec3(0);
 // The immutable working reference avoids reading neighboring pixels from
 // an image concurrently being updated by other composition invocations.
 for(int i=0;i<4;++i){vec2 offset=i==0?vec2(-1,0):i==1?vec2(1,0):i==2?vec2(0,-1):vec2(0,1);contrast=max(contrast,abs(sampleSmall(q+offset,false).rgb-smallOriginal));}
 float protect=1./(1.+6.*max(max(contrast.r,contrast.g),contrast.b));
 if(any(isnan(delta))||any(isinf(delta)))delta=vec3(0);
 imageStore(fullColor,p,vec4(clamp(original+pc.strength*protect*delta,0.,1.),1));}
)";
            auto nrSpirv=compiler.CompileGlslToSpv(nrShader,shaderc_compute_shader,"chiaki-nr-residual.comp");
            if(nrSpirv.GetCompilationStatus()!=shaderc_compilation_status_success)throw std::runtime_error(nrSpirv.GetErrorMessage());
            code.assign(nrSpirv.cbegin(),nrSpirv.cend());sm.codeSize=code.size()*4;sm.pCode=code.data();
            check(vkCreateShaderModule(vk->device,&sm,nullptr,&module),"NR composition shader");cp.stage.module=module;
            created=vkCreateComputePipelines(vk->device,VK_NULL_HANDLE,1,&cp,nullptr,&nrApplyPipeline);vkDestroyShaderModule(vk->device,module,nullptr);check(created,"NR composition pipeline");
            }
            if(!nrDescriptors)check(vkAllocateDescriptorSets(vk->device,&dai,&nrDescriptors),"NR descriptors");
            for(int i=0;i<3;++i){imageInfos[i].imageView=views[i==0?4:i==1?7:nrPasses==1?8:9];imageInfos[i].imageLayout=VK_IMAGE_LAYOUT_GENERAL;writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=nrDescriptors;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&imageInfos[i];}
            vkUpdateDescriptorSets(vk->device,3,writes.data(),0,nullptr);
            qInfo()<<"Native NR feature 18 active"<<nrWidth<<nrHeight<<"passes"<<nrPasses<<"style"<<nrStyle<<"intensity"<<nrIntensity;
        
    }
    void refreshNeural(){
        finish();for(auto &handle:nrFeatures){if(handle)nrRelease(handle);handle=nullptr;}if(nrCaps)core.destroy(nrCaps);nrCaps=nullptr;
        nrWidth=std::max(1,int((neuralBefore?width:outWidth)*nrScale+.5f));nrHeight=std::max(1,int((neuralBefore?height:outHeight)*nrScale+.5f));
        for(int i=7;i<=9;++i){if(views[i])vkDestroyImageView(vk->device,views[i],nullptr);views[i]=VK_NULL_HANDLE;pl_tex_destroy(vk->gpu,&textures[i]);
            pl_tex_params tp{};tp.w=nrWidth;tp.h=nrHeight;tp.format=pl_find_named_fmt(vk->gpu,"rgba16f");tp.sampleable=true;tp.storable=true;tp.blit_src=true;tp.blit_dst=true;tp.host_readable=QCoreApplication::arguments().contains("--nvidia-self-test");textures[i]=pl_tex_create(vk->gpu,&tp);if(!textures[i])throw std::runtime_error("NR image resize failed");
            VkFormat format;VkImage image=pl_vulkan_unwrap(vk->gpu,textures[i],&format,nullptr);VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(vkCreateImageView(vk->device,&vi,nullptr,&views[i]),"NR resized view");}
        createNeural();activeNrRevision=nrRevision;
    }
    void applyNeural(bool evaluateOnly=false){
        const int full=neuralBefore?0:4;
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;
        blit.srcOffsets[1]={textures[full]->params.w,textures[full]->params.h,1};blit.dstOffsets[1]={nrWidth,nrHeight,1};
        VkFormat fmt;
        vkCmdBlitImage(cmd,pl_vulkan_unwrap(vk->gpu,textures[full],&fmt,nullptr),VK_IMAGE_LAYOUT_GENERAL,pl_vulkan_unwrap(vk->gpu,textures[7],&fmt,nullptr),VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_LINEAR);
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        auto depth=resource(3),motion=resource(2);
        for(int pass=0;pass<nrPasses;++pass){
            auto color=resource(pass==0?7:8),output=resource(pass==0?8:9);
            int result=nrEval(cmd,nrFeatures[pass],nrCaps,&color,&depth,&motion,&output,nrWidth,nrHeight,width,height,width,height,0,0,0,0,0,!(history&&nrHistory),1.f,nrStyle,1.f,1.f,-1.f,1,1.f,1.f);
            if(nrEvaluations%15==0||result!=1)qInfo()<<"Native NR evaluate feature 18"<<Qt::hex<<unsigned(result)<<"pass"<<pass+1;
            if(result!=1)throw std::runtime_error("native NR feature 18 evaluation failed");
            ++nrEvaluations;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        }
        nrHistory=true;
        if(evaluateOnly)return;
        composeNeural();
    }
    void composeNeural(){
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,nrApplyPipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&nrDescriptors,0,nullptr);
        float effect=neuralBlend*nrIntensity;vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&effect);
        vkCmdDispatch(cmd,(outWidth+7)/8,(outHeight+7)/8,1);
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    }
};

NvidiaVideo::NvidiaVideo(QObject *parent):QObject(parent),d(new Impl){d->rtxCandidate=hasRtxVulkan();d->nrRuntimePresent=QFile::exists(QCoreApplication::applicationDirPath()+"/nvngx_dlssnr.dll");QSettings s;d->enabled=s.value("nvidia/sr",false).toBool();d->neural=d->rtxCandidate&&d->nrRuntimePresent&&s.value("nvidia/nr",false).toBool();d->quality=s.value("nvidia/quality",0).toInt();d->neuralBlend=s.value("nvidia/blend",1.).toFloat();d->fg=s.value("video/fg",false).toBool();d->renderPreset=s.value("nvidia/preset",0).toInt();auto config=neuralConfiguration();d->neuralBefore=config.value("before").toBool();d->nrScale=config.value("scale").toFloat();d->nrPasses=config.value("passes").toInt();d->nrStyle=config.value("style").toInt();d->nrIntensity=config.value("intensity").toFloat();d->nisSharpness=s.value("nvidia/nis_sharpness",.25).toFloat();
 d->finalSharp=0;d->uiFinalSharp=d->finalSharp;d->uiEnabled=d->enabled;d->uiNeural=d->neural;d->uiFg=d->fg;d->uiHybrid=d->hybrid;d->uiQuality=d->quality;d->uiPreset=d->renderPreset;d->uiBlend=d->neuralBlend;d->uiScale=d->nrScale;d->uiPasses=d->nrPasses;d->uiStyle=d->nrStyle;d->uiIntensity=d->nrIntensity;d->uiBefore=d->neuralBefore;d->uiNis=d->nisSharpness;}
NvidiaVideo::~NvidiaVideo(){shutdown();}
bool NvidiaVideo::fsr()const{QMutexLocker lock(&d->controlMutex);return d->uiFsr;}
void NvidiaVideo::setFsr(bool v){{QMutexLocker lock(&d->controlMutex);d->uiFsr=v;d->controlDirty|=512;}emit changed();}
double NvidiaVideo::finalSharpness()const{QMutexLocker lock(&d->controlMutex);return d->uiFinalSharp;}
void NvidiaVideo::setFinalSharpness(double v){v=std::clamp(v,0.,1.);{QMutexLocker lock(&d->controlMutex);d->uiFinalSharp=float(v);d->controlDirty|=1024;}QSettings().setValue("video/final_sharpness",v);emit changed();}
bool NvidiaVideo::enabled()const{QMutexLocker lock(&d->controlMutex);return d->uiEnabled;}
bool NvidiaVideo::neuralAvailable()const{QMutexLocker lock(&d->controlMutex);return d->rtxCandidate&&d->nrRuntimePresent&&!d->nrRejected;}
QString NvidiaVideo::neuralAvailabilityReason()const{QMutexLocker lock(&d->controlMutex);if(!d->rtxCandidate)return QStringLiteral("NR доступен только на совместимой NVIDIA RTX с Vulkan. Пространственные апскейлеры и RCAS остаются доступны.");if(!d->nrRuntimePresent)return QStringLiteral("Для NR добавьте совместимую nvngx_dlssnr.dll рядом с приложением и перезапустите его. Библиотека не входит в portable.");if(d->nrRejected)return QStringLiteral("Библиотека NR отклонена драйвером или выбранной видеокартой. Обычный стриминг сохранён.");return QStringLiteral("Найдены NVIDIA RTX и библиотека NR. Совместимость проверяется при первом включении; наличие RTX не гарантирует поддержку этой версии runtime.");}
bool NvidiaVideo::neural()const{QMutexLocker lock(&d->controlMutex);return d->uiNeural;}
bool NvidiaVideo::hybrid()const{QMutexLocker lock(&d->controlMutex);return d->uiHybrid;}
void NvidiaVideo::setHybrid(bool v){{QMutexLocker lock(&d->controlMutex);d->uiHybrid=v;d->controlDirty|=8;}emit changed();}
int NvidiaVideo::quality()const{QMutexLocker lock(&d->controlMutex);return d->uiQuality;}
QString NvidiaVideo::status()const{QMutexLocker lock(&d->controlMutex);return d->uiStatus;}
QVariantMap NvidiaVideo::gpuTimings()const{QMutexLocker lock(&d->controlMutex);return d->uiTimings;}
double NvidiaVideo::milliseconds()const{QMutexLocker lock(&d->controlMutex);return d->uiMs;}
void NvidiaVideo::setEnabled(bool v){{QMutexLocker lock(&d->controlMutex);d->uiEnabled=v;d->controlDirty|=1;}QSettings().setValue("nvidia/sr",v);emit changed();}
void NvidiaVideo::setNeural(bool v){if(v&&!neuralAvailable())return;{QMutexLocker lock(&d->controlMutex);d->uiNeural=v;d->controlDirty|=2;}QSettings().setValue("nvidia/nr",v);emit changed();}
void NvidiaVideo::setQuality(int v){v=std::clamp(v,0,3);{QMutexLocker lock(&d->controlMutex);d->uiQuality=v;d->controlDirty|=16;}QSettings().setValue("nvidia/quality",v);emit changed();}
double NvidiaVideo::neuralBlend()const{QMutexLocker lock(&d->controlMutex);return d->uiBlend;}
void NvidiaVideo::setNeuralBlend(double v){v=std::clamp(v,0.,1.);{QMutexLocker lock(&d->controlMutex);d->uiBlend=float(v);d->controlDirty|=64;}QSettings().setValue("nvidia/blend",v);emit changed();}
bool NvidiaVideo::frameGeneration()const{QMutexLocker lock(&d->controlMutex);return d->uiFg;}
QString NvidiaVideo::frameGenerationStatus()const{QMutexLocker lock(&d->controlMutex);return d->uiFgStatus;}
void NvidiaVideo::setFrameGeneration(bool v){{QMutexLocker lock(&d->controlMutex);d->uiFg=v;d->controlDirty|=4;d->uiFgStatus=v?QStringLiteral("Ожидание видеокадра"):QStringLiteral("Выключено");}QSettings().setValue("video/fg",v);emit changed();}
int NvidiaVideo::renderPreset()const{QMutexLocker lock(&d->controlMutex);return d->uiPreset;}
void NvidiaVideo::setRenderPreset(int v){v=(v==0||v==10||v==11||v==12||v==13)?v:0;{QMutexLocker lock(&d->controlMutex);d->uiPreset=v;d->controlDirty|=32;}QSettings().setValue("nvidia/preset",v);emit changed();}
bool NvidiaVideo::neuralBefore()const{QMutexLocker lock(&d->controlMutex);return d->uiBefore;}
double NvidiaVideo::dlssSharpness()const{QMutexLocker lock(&d->controlMutex);return d->uiNis;}
void NvidiaVideo::setDlssSharpness(double v){v=std::clamp(v,0.,1.);{QMutexLocker lock(&d->controlMutex);d->uiNis=float(v);d->controlDirty|=128;}QSettings().setValue("nvidia/nis_sharpness",v);emit changed();}
QVariantMap NvidiaVideo::neuralConfiguration() const{
    QSettings ini(QCoreApplication::applicationDirPath()+"/OptiScaler.ini",QSettings::IniFormat);
    auto number=[&](const char *key,double fallback){bool ok=false;double value=ini.value(key).toDouble(&ok);return ok&&std::isfinite(value)?value:fallback;};
    return {{"scale",std::clamp(number("DlssNr/WorkingScale",.7),.25,1.)}, {"passes",int(std::clamp(number("DlssNr/Passes",1),1.,2.))}, {"style",int(std::clamp(number("DlssNr/Style",0),0.,2.))}, {"intensity",std::clamp(number("DlssNr/Intensity",1.),0.,2.)}, {"before",ini.value("DlssNr/RunBeforeSR",true).toBool()}};
}
bool NvidiaVideo::saveNeuralConfiguration(double scale,int passes,int style,double intensity,bool before){
    QString path=QCoreApplication::applicationDirPath()+"/OptiScaler.ini";QFile input(path);
    if(!input.open(QIODevice::ReadOnly))return false;
    QString text=QString::fromUtf8(input.readAll());input.close();
    QRegularExpression section("(?m)^\\[DlssNr\\]\\s*$");auto match=section.match(text);if(!match.hasMatch())return false;
    int begin=match.capturedEnd();auto next=QRegularExpression("(?m)^\\[").match(text,begin);int end=next.hasMatch()?next.capturedStart():text.size();
    QString part=text.mid(begin,end-begin);
    auto set=[&](const QString &key,const QString &value){
        QRegularExpression re("(?m)^"+QRegularExpression::escape(key)+"\\s*=.*$");auto found=re.match(part);
        if(found.hasMatch())part.replace(found.capturedStart(),found.capturedLength(),key+"="+value);else part+="\n"+key+"="+value+"\n";
    };
    set("WorkingScale",QString::number(std::clamp(scale,.25,1.)));set("Passes",QString::number(std::clamp(passes,1,2)));set("Style",QString::number(std::clamp(style,0,2)));set("Intensity",QString::number(std::clamp(intensity,0.,2.)));set("RunBeforeSR",before?"true":"false");
    text.replace(begin,end-begin,part);
    if(!QFile::exists(path+".before-panel"))QFile::copy(path,path+".before-panel");
    QSaveFile output(path);if(!output.open(QIODevice::WriteOnly))return false;
    QByteArray bytes=text.toUtf8();if(output.write(bytes)!=bytes.size()||!output.commit())return false;
    {QMutexLocker control(&d->controlMutex);d->uiScale=std::clamp(float(scale),.25f,1.f);d->uiPasses=std::clamp(passes,1,2);d->uiStyle=std::clamp(style,0,2);d->uiIntensity=std::clamp(float(intensity),0.f,2.f);d->uiBefore=before;d->controlDirty|=256;}
    emit changed();return true;
}
void NvidiaVideo::noteInputFrame(){d->sequence.fetch_add(1,std::memory_order_relaxed);}
bool NvidiaVideo::retainsDeviceUntilExit() const{QMutexLocker lock(&d->mutex);return d->nr.module!=nullptr||d->core.module!=nullptr;}
void NvidiaVideo::shutdown(){QMutexLocker lock(&d->mutex);d->clear();if(d->vk&&!retainsDeviceUntilExit()&&d->core.initialized)d->core.shutdown(d->vk->device);d->vk=nullptr;d->queue=VK_NULL_HANDLE;d->core.initialized=false;d->nr.initialized=false;}

int NvidiaVideo::nextPhaseDelayMs() const{QMutexLocker lock(&d->mutex);if(!d->fg||!d->generatedValid||!d->presentAge.isValid())return 0;if(d->wakeForInput)return 1;if(!d->pendingReal)return 0;double remaining=d->phaseMs-d->presentAge.nsecsElapsed()/1e6;return remaining>0?std::max(1,int(std::ceil(remaining))):0;}

bool NvidiaVideo::render(pl_vulkan vk,pl_renderer renderer,const pl_frame *source,const pl_frame_mix *mix,const pl_frame *target,const pl_render_params *params,const pl_hook *spatialHook,float sharpness){
    QMutexLocker lock(&d->mutex);
    bool controlsChanged;{QMutexLocker controls(&d->controlMutex);controlsChanged=d->controlDirty!=0;}controlsChanged=controlsChanged||d->lastSharpness!=sharpness;
    d->consumeControls();
    if(controlsChanged)d->pendingReal=false;
    if(!d->enabled&&!d->neural&&!d->fg&&sharpness<=0&&d->lastSharpness<0){d->history=false;return false;}
    if(d->failed)return false;
    if(!vk){d->status=QStringLiteral("NVIDIA требует рендерер Vulkan; обычный стриминг сохранён");d->failed=true;d->publish();emit changed();return false;}
    if(d->neural){VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(vk->phys_device,&p);if(p.vendorID!=0x10de||!QString::fromUtf8(p.deviceName).contains("RTX",Qt::CaseInsensitive)){QMutexLocker controls(&d->controlMutex);d->neural=d->uiNeural=false;d->nrRejected=true;d->status=QStringLiteral("Выбранный GPU не поддерживает NR; используется обычная обработка.");}}
    if(!d->fg)d->pendingReal=false;
    if(d->fg&&d->pendingReal&&target&&d->history&&!d->forceFgValidation)return d->cachedPresent(renderer,target,params);
    d->wakeForInput=false;
    const pl_frame *input=source?source:(mix?pl_frame_mix_current(mix):nullptr);
    if(!target||target->num_planes<1)return false;
    if(!input||input->num_planes<1){
        // Overlay/FG timer redraws do not necessarily carry a new queue frame.
        // Falling through to an empty frame mix clears the screen to black.
        if(d->history&&d->textures[4]){
            pl_frame cached{};cached.num_planes=1;cached.planes[0].components=4;
            for(int i=0;i<4;++i)cached.planes[0].component_mapping[i]=i;
            cached.planes[0].texture=d->textures[d->fg&&d->generatedValid&&d->presentAge.isValid()&&d->presentAge.nsecsElapsed()<d->phaseMs*1e6?5:4];
            cached.repr=pl_color_repr_rgb;cached.repr.alpha=PL_ALPHA_NONE;cached.color=pl_color_space_srgb;
            cached.crop={0,0,float(d->outWidth),float(d->outHeight)};
            d->recordPresentation(cached.planes[0].texture==d->textures[5]);return pl_render_image(renderer,&cached,target,params);
        }
        return false;
    }
    int w=input->planes[0].texture->params.w,h=input->planes[0].texture->params.h;
    // Hardware decoders can allocate padded planes (for example 1088 rows for
    // a 1080-line stream). Use the visible crop for the DLSS working image.
    if(input->crop.x1>input->crop.x0)w=std::max(1,int(input->crop.x1-input->crop.x0));
    if(input->crop.y1>input->crop.y0)h=std::max(1,int(input->crop.y1-input->crop.y0));
    int ow=target->crop.x1>target->crop.x0?int(target->crop.x1-target->crop.x0):target->planes[0].texture->params.w;
    int oh=target->crop.y1>target->crop.y0?int(target->crop.y1-target->crop.y0):target->planes[0].texture->params.h;
    // The source is never reduced to fit a preset's nominal ratio.
    double ratio=std::min(double(ow)/w,double(oh)/h);ow=std::max(1,int(w*ratio));oh=std::max(1,int(h*ratio));
    if(ratio<=1.0&&!d->hybrid&&!d->neural&&!d->fg&&sharpness<=0&&d->lastSharpness<0){d->history=false;return false;}
    if(ratio<=1.0){ow=w;oh=h;}
    // Every processing path keeps the real output viewport, including FG without SR.
    std::vector<int> held;
    try{
        d->vk=vk;
        if(d->width!=w||d->height!=h||d->outWidth!=ow||d->outHeight!=oh||d->activeFsr!=d->fsr||d->activeHybrid!=d->hybrid||d->activeEnabled!=d->enabled||d->activeQuality!=d->quality||d->activeNeural!=d->neural||d->activeFg!=d->fg||d->activeRenderPreset!=d->renderPreset)d->init(w,h,ow,oh);
        else if(d->neural&&d->activeNrRevision!=d->nrRevision)d->refreshNeural();
        QElapsedTimer timer;timer.start();
        pl_frame intermediate{};intermediate.num_planes=1;intermediate.planes[0].texture=d->textures[0];intermediate.planes[0].components=4;intermediate.planes[0].component_mapping[0]=0;intermediate.planes[0].component_mapping[1]=1;intermediate.planes[0].component_mapping[2]=2;intermediate.planes[0].component_mapping[3]=3;
        intermediate.repr=pl_color_repr_rgb;intermediate.color=pl_color_space_srgb;
        // NGX/FFX color outputs do not promise meaningful alpha. Never let
        // an uninitialized alpha channel make a generated frame transparent.
        intermediate.repr.alpha=PL_ALPHA_NONE;
        intermediate.crop={0,0,float(w),float(h)};
        unsigned long long sequence=d->sequence.load(std::memory_order_relaxed);
        if(!source&&mix&&mix->signatures){
            for(int i=0;i<mix->num_frames;++i)if(mix->frames[i]==input){sequence=mix->signatures[i];break;}
        }
        if(d->history&&!controlsChanged&&d->completedSequence==sequence){
            pl_frame cached=intermediate;cached.planes[0].texture=d->textures[d->fg&&d->generatedValid&&d->presentAge.isValid()&&d->presentAge.nsecsElapsed()<d->phaseMs*1e6?5:4];cached.crop={0,0,float(ow),float(oh)};
            d->recordPresentation(cached.planes[0].texture==d->textures[5]);return pl_render_image(renderer,&cached,target,params);
        }
        if(source&&sequence>d->completedSequence+3)d->history=false;
        if(d->presentAge.isValid()&&d->presentAge.elapsed()>150)d->history=false;
        d->presentAge.start();
        pl_render_params clean=pl_render_default_params;clean.disable_linear_scaling=true;clean.hooks=nullptr;clean.num_hooks=0;
        // Feeding a blend of two decoded frames creates artificial double
        // edges before temporal reconstruction. Process one actual frame.
        if(!pl_render_image(renderer,input,&intermediate,&clean))throw std::runtime_error("input color conversion failed");
        const bool preparedSpatialBase=!d->enabled&&(ow!=w||oh!=h);
        if(preparedSpatialBase){
            pl_frame base=intermediate;base.planes[0].texture=d->textures[4];base.crop={0,0,float(ow),float(oh)};
            pl_render_params upscale=pl_render_high_quality_params;upscale.hooks=spatialHook?&spatialHook:nullptr;upscale.num_hooks=spatialHook?1:0;
            if(!pl_render_image(renderer,input,&base,&upscale))throw std::runtime_error("full resolution spatial base failed");
        }
        if(d->hybrid){
            pl_frame spatial=intermediate;spatial.planes[0].texture=d->textures[6];spatial.crop={0,0,float(ow),float(oh)};
            pl_render_params assist=pl_render_high_quality_params;assist.hooks=spatialHook?&spatialHook:nullptr;assist.num_hooks=spatialHook?1:0;
            if(!pl_render_image(renderer,input,&spatial,&assist))throw std::runtime_error("spatial assistance failed");
        }
        std::vector<uint64_t> waits;
        for(int i=0;i<16;++i){pl_vulkan_hold_params hp{};hp.tex=d->textures[i];hp.layout=VK_IMAGE_LAYOUT_GENERAL;hp.qf=VK_QUEUE_FAMILY_IGNORED;hp.semaphore={d->timeline,++d->serial};if(!pl_vulkan_hold_ex(vk->gpu,&hp))throw std::runtime_error("GPU texture handoff failed");held.push_back(i);waits.push_back(d->serial);}
        d->begin();if(d->timingPool)vkCmdResetQueryPool(d->cmd,d->timingPool,0,8);d->stamp(0);
        vkCmdBindPipeline(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->pipeline);vkCmdBindDescriptorSets(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->pipelineLayout,0,1,&d->descriptors,0,nullptr);
        int push[]={w,h,d->history&&(d->enabled||d->neural||d->fg)?0:1};vkCmdPushConstants(d->cmd,d->pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,12,push);vkCmdDispatch(d->cmd,(w+63)/64,(h+63)/64,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(d->cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        d->stamp(1);
        if(d->neural&&d->neuralBefore)d->applyNeural(true);
        d->stamp(2);
        if(d->hybrid){d->copyFull(6,4);d->sharpen(sharpness,sharpness,true);d->copyFull(4,6);vkCmdBindPipeline(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->scaleGuidePipeline);vkCmdBindDescriptorSets(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->pipelineLayout,0,1,&d->scaleGuideDescriptors,0,nullptr);vkCmdDispatch(d->cmd,(ow+7)/8,(oh+7)/8,1);d->imageBarrier();}
        if(d->parameters){
        auto color=d->resource(d->hybrid?6:0),motion=d->resource(d->hybrid?13:2),depth=d->resource(d->hybrid?14:3),output=d->resource(d->hybrid?15:4);
        ptr(d->parameters,NVSDK_NGX_Parameter_Color,&color);ptr(d->parameters,NVSDK_NGX_Parameter_MotionVectors,&motion);ptr(d->parameters,NVSDK_NGX_Parameter_Depth,&depth);ptr(d->parameters,NVSDK_NGX_Parameter_Output,&output);
        fp(d->parameters,NVSDK_NGX_Parameter_MV_Scale_X,1.f);fp(d->parameters,NVSDK_NGX_Parameter_MV_Scale_Y,1.f);fp(d->parameters,NVSDK_NGX_Parameter_Jitter_Offset_X,0.f);fp(d->parameters,NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f);
        fp(d->parameters,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,1000.f/60.f);fp(d->parameters,NVSDK_NGX_Parameter_DLSS_Pre_Exposure,1.f);fp(d->parameters,NVSDK_NGX_Parameter_DLSS_Exposure_Scale,1.f);si(d->parameters,NVSDK_NGX_Parameter_Reset,d->history?0:1);
        ui(d->parameters,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,d->srWidth);ui(d->parameters,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,d->srHeight);
        }
        auto result=NVSDK_NGX_Result_Success;
        if(d->enabled&&d->srOutWidth>d->srWidth){
            if(d->fsr){ffxDispatchDescUpscale up{};up.header.type=FFX_API_DISPATCH_DESC_TYPE_UPSCALE;up.commandList=d->cmd;up.color=d->ffxResource(d->hybrid?6:0);up.motionVectors=d->ffxResource(d->hybrid?13:2);up.depth=d->ffxResource(d->hybrid?14:3);up.output=d->ffxResource(d->hybrid?15:4);up.motionVectorScale={1,1};up.renderSize={unsigned(d->srWidth),unsigned(d->srHeight)};up.upscaleSize={unsigned(d->srOutWidth),unsigned(d->srOutHeight)};up.preExposure=1;up.frameTimeDelta=1000.f/60;up.reset=!d->history;up.cameraNear=.1;up.cameraFar=1000;up.cameraFovAngleVertical=1.04719755f;up.viewSpaceToMetersFactor=1;up.flags=FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;if(d->ffxDispatchWork(&d->fsrContext,&up.header))throw std::runtime_error("FSR upscale dispatch failed");}
            else result=d->api->evaluate(d->cmd,d->feature,d->parameters,nullptr);
        }
        else if(!preparedSpatialBase){
            VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;copy.extent={unsigned(w),unsigned(h),1};
            VkFormat fmt;vkCmdCopyImage(d->cmd,pl_vulkan_unwrap(vk->gpu,d->textures[0],&fmt,nullptr),VK_IMAGE_LAYOUT_GENERAL,pl_vulkan_unwrap(vk->gpu,d->textures[4],&fmt,nullptr),VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        }
        if(NVSDK_NGX_FAILED(result))throw std::runtime_error(("DLSS evaluate 0x"+QString::number(unsigned(result),16)).toStdString());
        if(d->hybrid){d->imageBarrier();vkCmdBindPipeline(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->downsamplePipeline);vkCmdBindDescriptorSets(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->pipelineLayout,0,1,&d->downsampleDescriptors,0,nullptr);vkCmdDispatch(d->cmd,(ow+7)/8,(oh+7)/8,1);d->imageBarrier();}
        if(d->enabled){d->imageBarrier();vkCmdBindPipeline(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->blendPipeline);vkCmdBindDescriptorSets(d->cmd,VK_PIPELINE_BIND_POINT_COMPUTE,d->pipelineLayout,0,1,&d->descriptors,0,nullptr);struct {float strength;int hybrid;} clamp{1.f,d->hybrid?1:0};vkCmdPushConstants(d->cmd,d->pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,&clamp);vkCmdDispatch(d->cmd,(ow+7)/8,(oh+7)/8,1);d->imageBarrier();}
        d->stamp(3);d->sharpen(sharpness);d->stamp(4);
        if(d->neural){if(d->neuralBefore)d->composeNeural();else d->applyNeural();}

        d->stamp(5);
        const bool testMode=d->forceFgValidation;
        const bool fgBudget=true; // Buffered presentation accepts processing beyond the half-frame budget.
        if(d->fg&&fgBudget){
            barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(d->cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            d->generate();
        }
        if(d->fg&&!fgBudget){d->generatedValid=false;d->fgHistory=false;++d->missedFgDeadlines;}
        d->stamp(6);if(d->fg)d->copyFull(4,12);d->stamp(7);
        d->submit(waits);d->readTimings();

        for(int i:held){pl_vulkan_release_params rp{};rp.tex=d->textures[i];rp.layout=VK_IMAGE_LAYOUT_GENERAL;rp.qf=VK_QUEUE_FAMILY_IGNORED;pl_vulkan_release_ex(vk->gpu,&rp);}held.clear();
        {pl_tex_blit_params blit{};blit.src=d->textures[0];blit.dst=d->textures[1];blit.sample_mode=PL_TEX_SAMPLE_NEAREST;pl_tex_blit(vk->gpu,&blit);}d->history=true;d->completedSequence=sequence;
        // Use only the remaining 60-Hz interval; expensive NR must not incur
        // another fixed half-frame stall on top of its own processing time.
        // Reserve 2 ms for the final render, timer rounding and submission.
        d->lastSharpness=sharpness;d->phaseMs=std::clamp(1000./60.-timer.nsecsElapsed()/1e6-2.,1.,8.333333);d->presentAge.start();d->pendingReal=d->fg&&d->generatedValid;
        pl_frame finalFrame=intermediate;finalFrame.planes[0].texture=d->textures[d->fg&&d->generatedValid&&d->presentAge.nsecsElapsed()<d->phaseMs*1e6?5:4];finalFrame.crop={0,0,float(ow),float(oh)};
        d->recordPresentation(finalFrame.planes[0].texture==d->textures[5]);
        bool ok=pl_render_image(renderer,&finalFrame,target,params);
        d->ms=timer.nsecsElapsed()/1e6;
        d->status=QStringLiteral("Исходный поток %1 × %2 → вывод %3 × %4%5").arg(w).arg(h).arg(ow).arg(oh).arg(d->neural?QStringLiteral(" · Vulkan NR %1 × %2 · %3 проход(а)").arg(d->nrWidth).arg(d->nrHeight).arg(d->nrPasses):QString());
        if(d->hybrid)d->status+=QStringLiteral(" · Advanced %1×%2 → %3 %4×%5 → Lanczos %1×%2").arg(ow).arg(oh).arg(d->fsr?"FSR 3.1":"DLSS").arg(d->srOutWidth).arg(d->srOutHeight);
        if(++d->processedFrames%15==1){qInfo()<<"Chiaki NVIDIA processing_ms"<<d->ms<<d->status;d->publish();emit changed();}
        return ok;
    }catch(const std::exception &e){
        // Do not destroy resources in flight following a timeout. The normal
        // rendering path remains available; teardown drains the queue first.
        if(!held.empty()){
            d->finish();
            for(int i:held){pl_vulkan_release_params rp{};rp.tex=d->textures[i];rp.layout=VK_IMAGE_LAYOUT_GENERAL;rp.qf=VK_QUEUE_FAMILY_IGNORED;pl_vulkan_release_ex(vk->gpu,&rp);}
        }
        d->status=QStringLiteral("NVIDIA отключён: ")+QString::fromUtf8(e.what());d->failed=true;if(d->neural){QMutexLocker controls(&d->controlMutex);d->nrRejected=true;d->uiNeural=false;}if(d->fg)d->fgStatus=QStringLiteral("Ошибка: ")+QString::fromUtf8(e.what());qWarning()<<d->status;d->publish();emit changed();return false;
    }
}

QByteArray NvidiaVideo::sharpeningShader(double strength){return QString::fromLatin1(R"RCAS(//!HOOK OUTPUT
//!BIND HOOKED
//!DESC AMD RCAS - Chiaki RGB adaptation
vec4 hook(){
 vec4 original=HOOKED_tex(HOOKED_pos);
 vec3 b=HOOKED_texOff(vec2(0,-1)).rgb,d=HOOKED_texOff(vec2(-1,0)).rgb,e=original.rgb,f=HOOKED_texOff(vec2(1,0)).rgb,h=HOOKED_texOff(vec2(0,1)).rgb;
 vec3 mn=min(min(b,d),min(f,h)),mx=max(max(b,d),max(f,h));
 vec3 hitMin=min(mn,e)/max(4.*mx,vec3(0.000001));
 vec3 hitMax=(1.-max(mx,e))/min(4.*mn-4.,vec3(-0.000001));
 vec3 lobes=max(vec3(-0.1875),min(max(-hitMin,hitMax),vec3(0)));
 float contrast=max(max(mx.r-mn.r,mx.g-mn.g),mx.b-mn.b);vec3 lumaWeights=vec3(.2126,.7152,.0722);float bL=dot(b,lumaWeights),dL=dot(d,lumaWeights),eL=dot(e,lumaWeights),fL=dot(f,lumaWeights),hL=dot(h,lumaWeights);float range=max(max(max(bL,dL),max(fL,hL)),eL)-min(min(min(bL,dL),min(fL,hL)),eL);float noise=1.-.5*clamp(abs(.25*(bL+dL+fL+hL)-eL)/max(range,.000001),0.,1.);
 float lobe=max(max(lobes.r,lobes.g),lobes.b)*%1*noise;
 vec3 rgb=(lobe*(b+d+f+h)+e)/(4.*lobe+1.);
 return vec4(clamp(rgb,0.,1.),original.a);
}
)RCAS").arg(QString::number(std::clamp(strength,0.,1.),'f',2)).toUtf8();}

bool NvidiaVideo::selfTest(const QString &reportPath){
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString &message){fprintf(stderr,"%s\n",message.toUtf8().constData());fflush(stderr);});
    QJsonObject report;report["fg_deadline_guard_bypassed_for_gpu_validation"]=true;report["test"]=QStringLiteral("synthetic GPU frames, not Remote Play");
    const char *exts[]={"VK_NVX_binary_import","VK_NVX_image_view_handle","VK_KHR_push_descriptor","VK_KHR_buffer_device_address","VK_KHR_external_memory_win32","VK_KHR_external_semaphore_win32"}; pl_vulkan_params vp=pl_vulkan_default_params;vp.allow_software=false;vp.opt_extensions=exts;vp.num_opt_extensions=6;
    pl_log_params lp{};lp.log_cb=pl_log_simple;lp.log_priv=stderr;lp.log_level=PL_LOG_WARN;pl_log log=pl_log_create(PL_API_VER,&lp);pl_vulkan vk=pl_vulkan_create(log,&vp);
    if(!vk){report["error"]="Vulkan unavailable";QFile f(reportPath);f.open(QIODevice::WriteOnly);f.write(QJsonDocument(report).toJson());return false;}
    pl_renderer renderer=pl_renderer_create(log,vk->gpu);
    pl_tex_params tp{};tp.w=1920;tp.h=1080;tp.format=pl_find_named_fmt(vk->gpu,"rgba8");tp.sampleable=true;tp.renderable=true;tp.host_writable=true;tp.blit_dst=true;tp.blit_src=true;
    pl_tex src=pl_tex_create(vk->gpu,&tp);tp.w=2560;tp.h=1440;tp.host_readable=true;pl_tex dst=pl_tex_create(vk->gpu,&tp);
    pl_frame source{};source.num_planes=1;source.planes[0].texture=src;source.planes[0].components=4;for(int i=0;i<4;++i)source.planes[0].component_mapping[i]=i;source.repr=pl_color_repr_rgb;source.color=pl_color_space_srgb;source.crop={0,0,1920,1080};
    pl_frame target=source;target.planes[0].texture=dst;target.crop={0,0,2560,1440};
    QJsonArray cases;bool success=true;NvidiaVideo processor;processor.d->forceFgValidation=true;
    std::vector<uint8_t> pixels(1920*1080*4);
    QByteArray baseline;
    for(bool neural:qEnvironmentVariableIsSet("CHIAKI_FG_ONLY")?std::vector<bool>{}:std::vector<bool>{false,true,false}){ qInfo()<<"starting case"<<neural;
        processor.d->enabled=true;processor.d->neural=neural;processor.d->failed=false;
        double sum=0,max=0;int passed=0;
        for(int frame=0;frame<24;++frame){
            for(int y=0;y<1080;++y)for(int x=0;x<1920;++x){int p=(y*1920+x)*4;pixels[p]=uint8_t(((x+frame*2)/24%2)*90+(x*150/1920));pixels[p+1]=uint8_t(y*255/1080);pixels[p+2]=uint8_t(((x/16+y/16)%2)*120+50);pixels[p+3]=255;}
            pl_tex_transfer_params upload{};upload.tex=src;upload.ptr=pixels.data();
            if(!pl_tex_upload(vk->gpu,&upload)){qWarning()<<"Test upload failed";break;}
            QElapsedTimer elapsed;elapsed.start();
            processor.noteInputFrame();
            bool ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);
            if(!ok)break;
            if(frame==0&&qEnvironmentVariableIsSet("CHIAKI_NATIVE_NR_PROBE")){
                QString directory=QCoreApplication::applicationDirPath();
                HMODULE forwarder=LoadLibraryExW(reinterpret_cast<const wchar_t*>((directory+"/chiaki_nvngx.dll").utf16()),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
                auto initialize=reinterpret_cast<int(*)(const wchar_t*,const wchar_t*,void*,void*,void*,int)>(GetProcAddress(forwarder,"dlssnr_vk_init"));
                auto create=reinterpret_cast<void*(*)(void*,void*,unsigned,unsigned,int,float,int,float,float,float,int,int)>(GetProcAddress(forwarder,"dlssnr_vk_create"));
                auto setFloatSlot=reinterpret_cast<void(*)(int)>(GetProcAddress(forwarder,"dlssnr_call_set_float_slot"));
                auto getCap=reinterpret_cast<NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter**)>(GetProcAddress(processor.d->core.module,"NVSDK_NGX_VULKAN_GetCapabilityParameters"));
                NVSDK_NGX_Parameter *cap=nullptr;
                int initialized=initialize?initialize(reinterpret_cast<const wchar_t*>((directory+"/nvngx_dlssnr.dll").utf16()),reinterpret_cast<const wchar_t*>(directory.utf16()),vk->instance,vk->phys_device,vk->device,0x15):-1;
                qInfo()<<"Native NR probe init"<<Qt::hex<<initialized;
                void *handle=nullptr;
                if(initialized==1&&getCap&&NVSDK_NGX_SUCCEED(getCap(&cap))){
                    setFloatSlot(6);processor.d->begin();
                    handle=create(processor.d->cmd,cap,2560,1440,0,1.f,0,1.f,1.f,-1.f,1,0);
                    qInfo()<<"Native NR probe create"<<handle;
                    processor.d->submit();
                    auto last=reinterpret_cast<int*>(GetProcAddress(forwarder,"dlssnr_vk_last_create"));
                    report["native_nr_create_result"]=last?QString::number(unsigned(*last),16):QStringLiteral("missing export");
                }
                report["native_nr_init_result"]=QString::number(unsigned(initialized),16);report["native_nr_create_ok"]=handle!=nullptr;
                QFile out(reportPath);if(out.open(QIODevice::WriteOnly))out.write(QJsonDocument(report).toJson());
                return handle!=nullptr;
            }
            pl_gpu_finish(vk->gpu);
            double milliseconds=elapsed.nsecsElapsed()/1e6;
            if(frame>3){sum+=milliseconds;max=std::max(max,milliseconds);}
            ++passed;
        }
        QByteArray readback(2560*1440*4,0);pl_tex_transfer_params download{};download.tex=dst;download.ptr=readback.data();bool downloaded=pl_tex_download(vk->gpu,&download);
        QJsonObject c;c["neural_requested"]=neural;c["dlss_frames"]=passed;c["status"]=processor.status();c["mean_processing_ms"]=passed>4?sum/(passed-4):0;c["max_processing_ms"]=max;c["gpu_stages_ms"]=QJsonObject::fromVariantMap(processor.d->timings);c["readback_ok"]=downloaded;
        if(downloaded){
            c["output_sha256"]=QString::fromLatin1(QCryptographicHash::hash(readback,QCryptographicHash::Sha256).toHex());
            QImage image(reinterpret_cast<const uchar*>(readback.constData()),2560,1440,QImage::Format_RGBA8888);
            image.save(reportPath+(neural?"-nr.png":"-sr.png"));
            if(!neural&&baseline.isEmpty())baseline=readback;
            if(neural&&!baseline.isEmpty()){
                int difference=0;for(int i=0;i<readback.size();++i)if(readback[i]!=baseline[i])++difference;
                c["different_bytes_from_sr"]=difference;
                QFile logFile(QCoreApplication::applicationDirPath()+"/OptiScaler.log");QByteArray evidence;if(logFile.open(QIODevice::ReadOnly))evidence=logFile.readAll();
                c["nr_verified"]=difference>0&&processor.d->nrEvaluations>0;
            }
        }
        cases.append(c);if(passed!=24||!downloaded)success=false;

    }
    QJsonArray transitions;
    for(int trial=0;trial<(qEnvironmentVariableIsSet("CHIAKI_FG_ONLY")?0:8);++trial){
        processor.d->enabled=trial!=0;processor.d->neural=(trial%2)==0;processor.d->quality=trial%3;processor.d->failed=false;
        processor.setNeuralBlend(trial==2?.5:1.0);
        int outputWidth=trial==0?1920:trial==1?2240:2560;
        int outputHeight=outputWidth*9/16;
        pl_gpu_finish(vk->gpu);pl_tex_destroy(vk->gpu,&dst);tp.w=outputWidth;tp.h=outputHeight;dst=pl_tex_create(vk->gpu,&tp);
        target.planes[0].texture=dst;target.crop={0,0,float(outputWidth),float(outputHeight)};
        bool ok=true;QElapsedTimer elapsed;elapsed.start();
        for(int frame=0;frame<6;++frame){processor.noteInputFrame();if(!processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params)){ok=false;break;}pl_gpu_finish(vk->gpu);}
        QJsonObject t;t["sr"]=processor.d->enabled;t["nr"]=processor.d->neural;t["quality"]=processor.d->quality;t["nr_blend"]=processor.neuralBlend();t["output_width"]=outputWidth;t["output_height"]=outputHeight;t["six_frames_ok"]=ok;t["total_ms_including_reinitialization"]=elapsed.elapsed();transitions.append(t);success=success&&ok;
    }
    QJsonArray fgReports;
    for(int mode=0;mode<(qEnvironmentVariableIsSet("CHIAKI_FG_ONLY")?1:3);++mode){
        processor.d->enabled=mode!=0;processor.d->neural=mode==2;processor.d->fg=true;processor.d->failed=false;
        bool fgOk=true;double fgSum=0;int fgFrames=0;unsigned long long firstFrame=processor.d->fgFrame;
        for(int frame=0;frame<18;++frame){
            for(int y=0;y<1080;++y)for(int x=0;x<1920;++x){int i=(y*1920+x)*4;pixels[i]=uint8_t(((x+frame*7)/32%2)*180);pixels[i+1]=uint8_t(y*255/1080);pixels[i+2]=80;pixels[i+3]=255;}
            pl_tex_transfer_params upload{};upload.tex=src;upload.ptr=pixels.data();pl_tex_upload(vk->gpu,&upload);pl_gpu_finish(vk->gpu);
            processor.noteInputFrame();QElapsedTimer elapsed;elapsed.start();
            if(!processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params)){fgOk=false;break;}
            pl_gpu_finish(vk->gpu);if(frame>3)fgSum+=elapsed.nsecsElapsed()/1e6;++fgFrames;
        }
        int difference=0;bool downloaded=false,notBlack=false;double realMean=0,generatedMean=0;
        if(fgOk){
            int size=processor.d->outWidth*processor.d->outHeight*processor.d->textures[4]->params.format->texel_size;
            qInfo()<<"FG readback bytes"<<size<<processor.d->outWidth<<processor.d->outHeight<<processor.d->textures[4]->params.format->texel_size;
            QByteArray real(size,0),generated(size,0);
            pl_tex_transfer_params download{};download.tex=processor.d->textures[4];download.ptr=real.data();bool a=pl_tex_download(vk->gpu,&download);
            download.tex=processor.d->textures[5];download.ptr=generated.data();downloaded=a&&pl_tex_download(vk->gpu,&download);
            if(downloaded){
                for(int i=0;i<size;++i)if(real[i]!=generated[i])++difference;
                const float *a=reinterpret_cast<const float*>(real.constData()),*b=reinterpret_cast<const float*>(generated.constData());
                int count=processor.d->outWidth*processor.d->outHeight;
                bool finite=true;QImage preview(processor.d->outWidth,processor.d->outHeight,QImage::Format_RGB32);
                for(int i=0;i<count;++i){
                    for(int c=0;c<3;++c){finite=finite&&std::isfinite(b[i*4+c]);realMean+=a[i*4+c];generatedMean+=b[i*4+c];}
                    preview.setPixel(i%preview.width(),i/preview.width(),qRgb(int(std::clamp(b[i*4],0.f,1.f)*255),int(std::clamp(b[i*4+1],0.f,1.f)*255),int(std::clamp(b[i*4+2],0.f,1.f)*255)));
                }
                realMean/=count*3;generatedMean/=count*3;notBlack=finite&&(realMean<.01||generatedMean>realMean*.25);
                preview.save(reportPath+QString("-fg-%1.png").arg(mode));
            }
        }
        QJsonObject fgReport;fgReport["sr"]=processor.d->enabled;fgReport["nr"]=processor.d->neural;fgReport["gpu_frames"]=fgFrames;fgReport["ok"]=fgOk;fgReport["status"]=processor.frameGenerationStatus();fgReport["mean_ms"]=fgFrames>4?fgSum/(fgFrames-4):0;
        fgReport["generated_readback_ok"]=downloaded;fgReport["generated_finite_not_black"]=notBlack;fgReport["generated_mean_rgb"]=generatedMean;fgReport["real_mean_rgb"]=realMean;fgReport["different_bytes_from_real_frame"]=difference;
        // A second presentation must reuse the image, never run the model twice.
        uint64_t count=processor.d->fgFrame;
        bool cachedOk=fgOk&&processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params)&&processor.d->fgFrame==count;
        fgReport["repeat_presentation_reuses_gpu_frame"]=cachedOk;
        pl_frame_mix empty{};bool overlayOk=processor.render(vk,renderer,nullptr,&empty,&target,&pl_render_default_params)&&processor.d->fgFrame==count;
        fgReport["empty_queue_overlay_retains_video"]=overlayOk;
        fgReports.append(fgReport);success=success&&fgOk&&downloaded&&difference>0&&cachedOk&&notBlack&&overlayOk;
    }
    report["fsr_fg"]=fgReports;
    if(processor.d->fg&&processor.d->generatedValid){uint64_t count=processor.d->fgFrame;std::this_thread::sleep_for(std::chrono::milliseconds(20));bool ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);report["second_presentation_phase_without_generation"]=ok&&processor.d->fgFrame==count&&processor.d->lastRealPresent==count;success=success&&ok&&processor.d->lastRealPresent==count;}

    QJsonArray liveProfiles;
    if(!qEnvironmentVariableIsSet("CHIAKI_FG_ONLY")){
        QFile original(QCoreApplication::applicationDirPath()+"/OptiScaler.ini");QByteArray originalBytes;if(original.open(QIODevice::ReadOnly)){originalBytes=original.readAll();original.close();}
        processor.d->enabled=true;processor.d->neural=true;processor.d->fg=false;processor.d->neuralBlend=1.f;processor.d->failed=false;
        for(int profile=0;profile<6;++profile){
            const double scales[]={.25,.5,.75,1.,.5,.7};double scale=scales[profile];int passes=profile==4?2:1;
            bool saved=processor.saveNeuralConfiguration(scale,passes,profile==5?0:profile%3,profile==5?2.:.8,profile==4);
            bool ok=saved;double sum=0;
            for(int i=0;i<4&&ok;++i){processor.noteInputFrame();QElapsedTimer elapsed;elapsed.start();ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);pl_gpu_finish(vk->gpu);if(i>0)sum+=elapsed.nsecsElapsed()/1e6;}
            QByteArray result(dst->params.w*dst->params.h*4,0);pl_tex_transfer_params dl{};dl.tex=dst;dl.ptr=result.data();bool downloaded=ok&&pl_tex_download(vk->gpu,&dl);
            int expectedW=int((profile==4?1920:target.crop.x1)*scale+.5);
            ok=ok&&processor.d->width==1920&&processor.d->height==1080&&processor.d->nrWidth==expectedW&&processor.d->nrPasses==passes&&processor.d->activeNrRevision==processor.d->nrRevision;
            QJsonObject p;p["scale"]=scale;p["intensity"]=processor.d->nrIntensity;p["style"]=processor.d->nrStyle;p["blend"]=processor.d->neuralBlend;p["passes"]=passes;p["before_sr"]=profile==4;p["nr_width"]=processor.d->nrWidth;p["nr_height"]=processor.d->nrHeight;p["mean_ms_after_recreation"]=sum/3.;p["applied_live"]=ok;p["gpu_stages_ms"]=QJsonObject::fromVariantMap(processor.d->timings);p["sha256"]=QString::fromLatin1(QCryptographicHash::hash(result,QCryptographicHash::Sha256).toHex());
            liveProfiles.append(p);success=success&&ok&&downloaded;
        }
        if(!originalBytes.isEmpty()&&original.open(QIODevice::WriteOnly)){original.write(originalBytes);original.close();}
    }
    report["live_nr_profiles"]=liveProfiles;
    if(!qEnvironmentVariableIsSet("CHIAKI_FG_ONLY")){
        QFile file(QCoreApplication::applicationDirPath()+"/OptiScaler.ini");QByteArray saved;if(file.open(QIODevice::ReadOnly)){saved=file.readAll();file.close();}
        processor.d->enabled=true;processor.d->neural=true;processor.d->fg=false;processor.d->failed=false;
        std::atomic<bool> heldLock{false};std::thread busy([&]{QMutexLocker lock(&processor.d->mutex);heldLock.store(true);std::this_thread::sleep_for(std::chrono::milliseconds(150));});
        while(!heldLock.load())std::this_thread::yield();QElapsedTimer accept;accept.start();
        bool accepted=processor.saveNeuralConfiguration(.25,2,2,1.,true)&&processor.saveNeuralConfiguration(.75,1,1,1.,false)&&processor.saveNeuralConfiguration(.5,1,0,1.,false);
        double acceptMs=accept.nsecsElapsed()/1e6;busy.join();auto srHandle=processor.d->feature;unsigned builds=processor.d->nrRebuilds;
        processor.noteInputFrame();bool ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);pl_gpu_finish(vk->gpu);
        bool lastOnly=ok&&processor.d->nrScale==.5f&&processor.d->nrPasses==1&&processor.d->nrStyle==0&&!processor.d->neuralBefore&&processor.d->nrRebuilds==builds+1&&processor.d->feature==srHandle;
        builds=processor.d->nrRebuilds;bool intensity=processor.saveNeuralConfiguration(.5,1,0,1.5,false);processor.noteInputFrame();intensity=intensity&&processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params)&&processor.d->nrRebuilds==builds;
        QJsonObject controls;controls["accept_ms_while_gpu_locked"]=acceptMs;controls["latest_only_one_nr_rebuild_sr_preserved"]=lastOnly;controls["intensity_without_rebuild"]=intensity;report["nonblocking_nr_controls"]=controls;success=success&&accepted&&acceptMs<100&&lastOnly&&intensity;
        if(!saved.isEmpty()&&file.open(QIODevice::WriteOnly)){file.write(saved);file.close();}
    }

    if(!qEnvironmentVariableIsSet("CHIAKI_FG_ONLY")){
        // Exercise the hybrid using the same two-plane YUV representation as
        // a decoded stream. A LUMA-hook test on an RGB texture is insufficient.
        pl_tex_params ytp{};ytp.w=1920;ytp.h=1080;ytp.format=pl_find_named_fmt(vk->gpu,"r8");ytp.sampleable=true;ytp.host_writable=true;
        pl_tex yTex=pl_tex_create(vk->gpu,&ytp);ytp.w=960;ytp.h=540;ytp.format=pl_find_named_fmt(vk->gpu,"rg8");pl_tex uvTex=pl_tex_create(vk->gpu,&ytp);
        std::vector<uint8_t> yPixels(1920*1080),uvPixels(960*540*2);
        for(int y=0;y<1080;++y)for(int x=0;x<1920;++x)yPixels[y*1920+x]=uint8_t(40+(((x/3+y/5)%2)*100)+(y*60/1080));
        for(size_t i=0;i<uvPixels.size();i+=2){uvPixels[i]=115;uvPixels[i+1]=140;}
        pl_tex_transfer_params up{};up.tex=yTex;up.ptr=yPixels.data();bool uploaded=pl_tex_upload(vk->gpu,&up);up.tex=uvTex;up.ptr=uvPixels.data();uploaded=uploaded&&pl_tex_upload(vk->gpu,&up);
        pl_frame yuv=source;yuv.num_planes=2;yuv.planes[0].texture=yTex;yuv.planes[0].components=1;yuv.planes[0].component_mapping[0]=0;yuv.planes[1].texture=uvTex;yuv.planes[1].components=2;yuv.planes[1].component_mapping[0]=1;yuv.planes[1].component_mapping[1]=2;yuv.repr=pl_color_repr_hdtv;yuv.color=pl_color_space_bt709;
        QFile shaderFile(":/shaders/FSRCNNX_x2_16-0-4-1.glsl");QByteArray cnn;if(shaderFile.open(QIODevice::ReadOnly))cnn=shaderFile.readAll();const pl_hook *spatial=pl_mpv_user_shader_parse(vk->gpu,cnn.constData(),cnn.size());
        processor.d->enabled=true;processor.d->neural=false;processor.d->fg=false;processor.d->failed=false;
        QByteArray srPixels(dst->params.w*dst->params.h*4,0),hybridPixels(srPixels.size(),0);
        bool ok=uploaded&&spatial;processor.setHybrid(false);
        for(int i=0;i<4&&ok;++i){processor.noteInputFrame();ok=processor.render(vk,renderer,&yuv,nullptr,&target,&pl_render_default_params);}
        pl_tex_transfer_params dl{};dl.tex=dst;dl.ptr=srPixels.data();ok=ok&&pl_tex_download(vk->gpu,&dl);
        processor.setHybrid(true);
        for(int i=0;i<4&&ok;++i){processor.noteInputFrame();ok=processor.render(vk,renderer,&yuv,nullptr,&target,&pl_render_default_params,spatial);}
        dl.ptr=hybridPixels.data();ok=ok&&pl_tex_download(vk->gpu,&dl);
        QJsonObject hybrid;hybrid["source_width"]=processor.d->width;hybrid["source_height"]=processor.d->height;hybrid["target_width"]=processor.d->outWidth;hybrid["target_height"]=processor.d->outHeight;hybrid["yuv_spatial_hook_loaded"]=spatial!=nullptr;hybrid["sequential_supersampling_changes_pixels"]=srPixels!=hybridPixels;hybrid["ok"]=ok;
        hybrid["sr_input_width"]=processor.d->srWidth;hybrid["sr_input_height"]=processor.d->srHeight;hybrid["supersample_width"]=processor.d->srOutWidth;hybrid["supersample_height"]=processor.d->srOutHeight;report["hybrid_dlss_spatial"]=hybrid;success=success&&ok&&srPixels!=hybridPixels&&processor.d->width==1920&&processor.d->height==1080;
        QImage(reinterpret_cast<const uchar*>(hybridPixels.constData()),dst->params.w,dst->params.h,QImage::Format_RGBA8888).save(reportPath+"-hybrid.png");
        QJsonArray fsrTests;
        for(bool combined:{false,true}){processor.setFsr(true);processor.setHybrid(combined);bool fsrOk=true;for(int i=0;i<5&&fsrOk;++i){processor.noteInputFrame();fsrOk=processor.render(vk,renderer,&yuv,nullptr,&target,&pl_render_default_params,combined?spatial:nullptr);}
            QByteArray out(srPixels.size(),0);dl.ptr=out.data();fsrOk=fsrOk&&pl_tex_download(vk->gpu,&dl);int lit=0;for(int i=0;i<out.size();i+=4)if(uint8_t(out[i])>8)++lit;
            QJsonObject f;f["combined"]=combined;f["native_fsr_context"]=processor.d->fsrContext!=nullptr;f["input_width"]=processor.d->srWidth;f["input_height"]=processor.d->srHeight;f["sr_output_width"]=processor.d->srOutWidth;f["sr_output_height"]=processor.d->srOutHeight;f["final_width"]=processor.d->outWidth;f["final_height"]=processor.d->outHeight;f["nonblack"]=lit>out.size()/32;f["gpu_stages_ms"]=QJsonObject::fromVariantMap(processor.d->timings);f["ok"]=fsrOk;fsrTests.append(f);success=success&&fsrOk&&processor.d->fsrContext&&lit>out.size()/32;
            QImage(reinterpret_cast<const uchar*>(out.constData()),dst->params.w,dst->params.h,QImage::Format_RGBA8888).save(reportPath+(combined?"-advanced-fsr.png":"-fsr.png"));
        }report["native_fsr_upscaling"]=fsrTests;processor.setFsr(false);
        QFile configFile(QCoreApplication::applicationDirPath()+"/OptiScaler.ini");QByteArray savedConfig;if(configFile.open(QIODevice::ReadOnly)){savedConfig=configFile.readAll();configFile.close();}
        processor.setHybrid(false);processor.d->enabled=false;processor.d->neural=true;processor.d->fg=false;processor.d->failed=false;
        bool spatialNrOk=processor.saveNeuralConfiguration(.5,1,0,1.,false);
        QByteArray spatialBase(srPixels.size(),0),spatialNr(srPixels.size(),0);
        processor.setNeuralBlend(0.);
        for(int i=0;i<4&&spatialNrOk;++i){processor.noteInputFrame();spatialNrOk=processor.render(vk,renderer,&yuv,nullptr,&target,&pl_render_default_params,spatial);}
        dl.ptr=spatialBase.data();spatialNrOk=spatialNrOk&&pl_tex_download(vk->gpu,&dl);
        processor.setNeuralBlend(.5);
        for(int i=0;i<4&&spatialNrOk;++i){processor.noteInputFrame();spatialNrOk=processor.render(vk,renderer,&yuv,nullptr,&target,&pl_render_default_params,spatial);}
        dl.ptr=spatialNr.data();spatialNrOk=spatialNrOk&&pl_tex_download(vk->gpu,&dl);
        double originalDetail=0,enhancedDetail=0;
        for(int y=0;y<dst->params.h;++y)for(int x=1;x<dst->params.w;++x){int i=(y*dst->params.w+x)*4;originalDetail+=std::abs(int(uint8_t(spatialBase[i]))-int(uint8_t(spatialBase[i-4])));enhancedDetail+=std::abs(int(uint8_t(spatialNr[i]))-int(uint8_t(spatialNr[i-4])));}
        double detailRatio=originalDetail>0?enhancedDetail/originalDetail:0;
        QJsonObject spatialNR;spatialNR["ok"]=spatialNrOk;spatialNR["original_width"]=processor.d->width;spatialNR["output_width"]=processor.d->outWidth;spatialNR["output_height"]=processor.d->outHeight;spatialNR["nr_width"]=processor.d->nrWidth;spatialNR["nr_height"]=processor.d->nrHeight;spatialNR["blend_changes_pixels"]=spatialBase!=spatialNr;spatialNR["retained_detail_ratio"]=detailRatio;report["spatial_then_nr"]=spatialNR;
        success=success&&spatialNrOk&&spatialBase!=spatialNr&&processor.d->outWidth==2560&&processor.d->nrWidth==1280&&detailRatio>.75;
        QImage(reinterpret_cast<const uchar*>(spatialNr.constData()),dst->params.w,dst->params.h,QImage::Format_RGBA8888).save(reportPath+"-spatial-nr.png");
        if(!savedConfig.isEmpty()&&configFile.open(QIODevice::WriteOnly)){configFile.write(savedConfig);configFile.close();}
        pl_mpv_user_shader_destroy(&spatial);pl_tex_destroy(vk->gpu,&yTex);pl_tex_destroy(vk->gpu,&uvTex);processor.setHybrid(false);
    }
    if(!qEnvironmentVariableIsSet("CHIAKI_FG_ONLY")){
        processor.d->enabled=true;processor.d->neural=false;processor.d->fg=false;processor.d->failed=false;
        QByteArray unsharp(dst->params.w*dst->params.h*4,0),sharpened(unsharp.size(),0);processor.setDlssSharpness(0.);
        bool ok=true;for(int i=0;i<4&&ok;++i){processor.noteInputFrame();ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);}pl_tex_transfer_params dl{};dl.tex=dst;dl.ptr=unsharp.data();ok=ok&&pl_tex_download(vk->gpu,&dl);
        processor.setDlssSharpness(.75);for(int i=0;i<4&&ok;++i){processor.noteInputFrame();ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);}dl.ptr=sharpened.data();ok=ok&&pl_tex_download(vk->gpu,&dl);
        report["official_nis_sharpness_changes_output"]=ok&&unsharp!=sharpened;success=success&&ok&&unsharp!=sharpened;
        QImage fixture(qEnvironmentVariable("CHIAKI_VIDEO_FIXTURE"));
        if(!fixture.isNull()){
            // Static and known horizontal pan of a photographic fixture expose
            // noise/inpainting errors that a smooth gradient cannot reveal.
            fixture=fixture.copy(0,0,fixture.width()*2/3,fixture.height()).scaled(1920,1080,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
            processor.d->enabled=false;processor.d->neural=false;processor.d->fg=true;processor.d->failed=false;QJsonArray videoCases;
            for(int scenario=0;scenario<2;++scenario){processor.d->history=false;processor.d->fgHistory=false;bool ok=true;
                for(int frame=0;frame<18&&ok;++frame){int offset=scenario?frame*4:0;for(int y=0;y<1080;++y)for(int x=0;x<1920;++x){const uchar *pixel=fixture.constScanLine(y)+((x+offset)%1920)*4;int p=(y*1920+x)*4;for(int c=0;c<4;++c)pixels[p+c]=pixel[c];}pl_tex_transfer_params up{};up.tex=src;up.ptr=pixels.data();ok=pl_tex_upload(vk->gpu,&up);processor.noteInputFrame();ok=ok&&processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);}
                QByteArray generated(dst->params.w*dst->params.h*4,0),expected(generated.size(),0);
                pl_frame gen=source;gen.planes[0].texture=processor.d->textures[5];gen.repr.alpha=PL_ALPHA_NONE;gen.crop={0,0,float(processor.d->outWidth),float(processor.d->outHeight)};ok=ok&&pl_render_image(renderer,&gen,&target,&pl_render_default_params);dl.ptr=generated.data();ok=ok&&pl_tex_download(vk->gpu,&dl);
                int expectedOffset=scenario?17*4-2:0;
                for(int y=0;y<1080;++y)for(int x=0;x<1920;++x){const uchar *pixel=fixture.constScanLine(y)+((x+expectedOffset)%1920)*4;int p=(y*1920+x)*4;for(int c=0;c<4;++c)pixels[p+c]=pixel[c];}pl_tex_transfer_params up{};up.tex=src;up.ptr=pixels.data();ok=ok&&pl_tex_upload(vk->gpu,&up);ok=ok&&pl_render_image(renderer,&source,&target,&pl_render_high_quality_params);dl.ptr=expected.data();ok=ok&&pl_tex_download(vk->gpu,&dl);
                double error=0;int big=0,count=0;for(int y=8;y<dst->params.h-8;++y)for(int x=32;x<dst->params.w-128;++x)for(int c=0;c<3;++c){int p=(y*dst->params.w+x)*4+c;int difference=std::abs(int(uint8_t(generated[p]))-int(uint8_t(expected[p])));error+=difference/255.;big+=difference>40;++count;}
                error/=std::max(1,count);double outliers=double(big)/std::max(1,count);
                QJsonObject c;c["scenario"]=scenario?"known 4-pixel pan, expected halfway":"static photographic frame";c["ok"]=ok;c["mean_error_to_expected"]=error;c["large_error_fraction"]=outliers;c["real_phases"]=double(processor.d->realPresents);c["generated_phases"]=double(processor.d->generatedPresents);videoCases.append(c);success=success&&ok&&(scenario?error<.065:error<.01);
                QImage(reinterpret_cast<const uchar*>(generated.constData()),dst->params.w,dst->params.h,QImage::Format_RGBA8888).save(reportPath+QString("-video-fg-%1.png").arg(scenario));
            }
            report["photographic_video_fg"]=videoCases;
        }else report["photographic_fixture_unavailable"]=true;
    }
    auto shader=sharpeningShader(.7);
    const pl_hook *hook=pl_mpv_user_shader_parse(vk->gpu,shader.constData(),shader.size());
    pl_render_params sharpen=pl_render_default_params;sharpen.hooks=&hook;sharpen.num_hooks=1;
    bool sharpOk=hook&&pl_render_image(renderer,&source,&target,&sharpen);pl_gpu_finish(vk->gpu);
    report["rcas_shader_gpu_render_ok"]=sharpOk;success=success&&sharpOk;pl_mpv_user_shader_destroy(&hook);
    processor.d->fg=false;
    processor.d->enabled=false;processor.d->neural=false;processor.d->lastSharpness=-1;report["both_disabled_bypass_ok"]=!processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);
    report["transitions"]=transitions;
    {
        processor.setFsr(false);processor.setHybrid(false);processor.d->enabled=false;processor.d->neural=false;processor.d->fg=true;processor.d->failed=false;processor.d->pendingReal=false;processor.d->wakeForInput=false;processor.d->forceFgValidation=false;
        // Force a known missed deadline without changing the real production guard.
        processor.d->estimatedCoreMs=20;processor.d->estimatedFgMs=3;auto frameID=processor.d->fgFrame;auto misses=processor.d->missedFgDeadlines;processor.noteInputFrame();bool ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);
        for(int warm=0;warm<3&&ok&&!processor.d->generatedValid;++warm){processor.noteInputFrame();ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);}
        bool buffered=ok&&processor.d->generatedValid&&processor.d->pendingReal;auto generatedCount=processor.d->generatedPresents;auto id=processor.d->fgFrame;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));processor.noteInputFrame();bool shown=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_default_params);
        bool paired=shown&&processor.d->fgFrame==id&&!processor.d->pendingReal&&processor.d->lastRealPresent==id&&processor.d->wakeForInput;
        report["production_buffered_fg_pair"]=buffered&&paired;report["production_pair_initial_generated"]=buffered;report["production_pair_real_phase"]=paired;success=success&&buffered&&paired;
        processor.d->forceFgValidation=true;
    }

    {
        processor.setEnabled(false);processor.setNeural(false);processor.setFrameGeneration(false);processor.setFsr(false);processor.setHybrid(false);processor.d->failed=false;
        QByteArray plain(dst->params.w*dst->params.h*4,0),sharp(plain.size(),0),nrOne(plain.size(),0),nrTwo(plain.size(),0);
        auto download=[&](QByteArray &bytes){pl_gpu_finish(vk->gpu);pl_tex_transfer_params dl{};dl.tex=dst;dl.ptr=bytes.data();return pl_tex_download(vk->gpu,&dl);};
        auto diff=[](const QByteArray &a,const QByteArray &b){double sum=0;for(int i=0;i<a.size();++i)if(i%4!=3)sum+=std::abs(int((unsigned char)a[i])-int((unsigned char)b[i]));return sum/(a.size()*0.75*255.);};
        // Same input sequence: an overlay slider change must invalidate cache.
        processor.noteInputFrame();bool ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_high_quality_params,nullptr,.001f)&&download(plain);
        ok=ok&&processor.render(vk,renderer,&source,nullptr,&target,&pl_render_high_quality_params,nullptr,1.f)&&download(sharp);
        double rcasDiff=diff(plain,sharp);report["rcas_plain_spatial_live_difference"]=rcasDiff;report["rcas_plain_spatial_live_changes_pixels"]=ok&&rcasDiff>.00001;success=success&&ok&&rcasDiff>.00001;
        processor.setNeural(true);processor.saveNeuralConfiguration(.7,1,0,1.,true);processor.d->nrHistory=false;processor.noteInputFrame();
        ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_high_quality_params,nullptr,1.f)&&download(nrOne);
        processor.saveNeuralConfiguration(.7,1,0,2.,true);processor.d->nrHistory=false;
        ok=ok&&processor.render(vk,renderer,&source,nullptr,&target,&pl_render_high_quality_params,nullptr,1.f)&&download(nrTwo);
        double deltaOne=diff(sharp,nrOne),deltaTwo=diff(sharp,nrTwo);report["nr_effect_intensity_1_delta"]=deltaOne;report["nr_effect_intensity_2_delta"]=deltaTwo;report["nr_effect_intensity_changes_pixels"]=ok&&nrOne!=nrTwo&&deltaTwo>deltaOne*1.1;success=success&&ok&&nrOne!=nrTwo&&deltaTwo>deltaOne*1.1;
        processor.setNeural(false);processor.setFrameGeneration(true);processor.d->forceFgValidation=true;double total=0;QVariantMap stages;
        for(int i=0;i<20&&ok;++i){processor.noteInputFrame();QElapsedTimer elapsed;elapsed.start();ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_high_quality_params,nullptr,1.f);pl_gpu_finish(vk->gpu);if(i>=4)total+=elapsed.nsecsElapsed()/1e6;stages=processor.d->timings;}
        report["video_optical_flow_backend"]=ok&&processor.d->generatedValid&&processor.d->ffx==nullptr;report["video_fg_with_rcas_mean_processing_ms"]=total/16.;report["video_fg_gpu_stages_ms"]=QJsonObject::fromVariantMap(stages);success=success&&ok&&processor.d->generatedValid&&processor.d->ffx==nullptr;
    }
    {
        processor.setNeural(false);processor.setFrameGeneration(true);processor.d->forceFgValidation=false;processor.d->history=false;processor.d->pendingReal=false;
        QElapsedTimer clock;clock.start();bool ok=true;uint64_t initialGenerated=processor.d->generatedPresents,initialReal=processor.d->realPresents;
        for(int i=0;i<40&&ok;++i){double remaining=i*(1000./60.)-clock.nsecsElapsed()/1e6;if(remaining>0)std::this_thread::sleep_for(std::chrono::microseconds(int(remaining*1000)));
            processor.noteInputFrame();ok=processor.render(vk,renderer,&source,nullptr,&target,&pl_render_high_quality_params,nullptr,1.f);
            if(processor.d->pendingReal){int delay=processor.nextPhaseDelayMs();std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1,delay)));ok=ok&&processor.render(vk,renderer,&source,nullptr,&target,&pl_render_high_quality_params,nullptr,1.f);}
        }
        QJsonObject cadence;cadence["ok"]=ok;cadence["seconds"]=clock.nsecsElapsed()/1e9;cadence["source_frames"]=40;cadence["generated_phases"]=qint64(processor.d->generatedPresents-initialGenerated);cadence["real_phases"]=qint64(processor.d->realPresents-initialReal);cadence["physical_monitor_fps_measured"]=false;report["production_60hz_cadence"]=cadence;
        success=success&&ok&&processor.d->generatedPresents-initialGenerated>=35&&processor.d->realPresents-initialReal>=35;
    }
    report["cases"]=cases;report["limitations"]=QStringLiteral("Синтетические кадры. Оценённые motion vectors; нейтральная глубина. Это не исходные данные PS5. NR подтверждён журналом feature 18 и изменением пикселей.");
    const bool retained=processor.retainsDeviceUntilExit();processor.shutdown();pl_renderer_destroy(&renderer);pl_tex_destroy(vk->gpu,&src);pl_tex_destroy(vk->gpu,&dst);if(!retained)pl_vulkan_destroy(&vk);pl_log_destroy(&log);
    report["device_retained_until_process_exit"]=retained;
    QFile f(reportPath);f.open(QIODevice::WriteOnly);f.write(QJsonDocument(report).toJson());return success;
}
