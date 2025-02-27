/*
 * Copyright (c) 2017-2024 The Forge Interactive Inc.
 *
 * This is a part of Ephemeris.
 * This file(code) is licensed under a Creative Commons Attribution-NonCommercial 4.0 International License
 * (https://creativecommons.org/licenses/by-nc/4.0/legalcode) Based on a work at https://github.com/ConfettiFX/The-Forge. You can not use
 * this code for commercial purposes.
 *
 */

// Ephemeris BEGIN
#include "../../Sky/src/Sky.h"
#include "../../SpaceObjects/src/SpaceObjects.h"
#include "../../Terrain/src/Terrain.h"
#include "../../VolumetricClouds/src/VolumetricClouds.h"
// Ephemeris END

// Interfaces
#include "../../../../The-Forge/Common_3/Application/Interfaces/IApp.h"
#include "../../../../The-Forge/Common_3/Application/Interfaces/ICameraController.h"
#include "../../../../The-Forge/Common_3/Application/Interfaces/IFont.h"
#include "../../../../The-Forge/Common_3/Application/Interfaces/IInput.h"
#include "../../../../The-Forge/Common_3/Application/Interfaces/IProfiler.h"
#include "../../../../The-Forge/Common_3/Application/Interfaces/IScreenshot.h"
#include "../../../../The-Forge/Common_3/Application/Interfaces/IUI.h"
#include "../../../../The-Forge/Common_3/Game/Interfaces/IScripting.h"
#include "../../../../The-Forge/Common_3/Utilities/Interfaces/IFileSystem.h"
#include "../../../../The-Forge/Common_3/Utilities/Interfaces/ILog.h"
#include "../../../../The-Forge/Common_3/Utilities/Interfaces/ITime.h"

// Renderer
#include "../../../../The-Forge/Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../The-Forge/Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"

#include "../../../../The-Forge/Common_3/Utilities/RingBuffer.h"

// Math
#include "../../../../The-Forge/Common_3/Utilities/Math/MathTypes.h"

// Memory
#include "../../../../The-Forge/Common_3/Utilities/Interfaces/IMemory.h"

#define FOREACH_SETTING(X)    X(InsufficientBindlessEntries, 0)

#define GENERATE_ENUM(x, y)   x,
#define GENERATE_STRING(x, y) #x,
#define GENERATE_STRUCT(x, y) uint32_t m##x = y;

typedef enum ESettings
{
    FOREACH_SETTING(GENERATE_ENUM) Count
} ESettings;

const char* gSettingNames[] = { FOREACH_SETTING(GENERATE_STRING) };

// Useful for using names directly instead of subscripting an array
struct ConfigSettings
{
    FOREACH_SETTING(GENERATE_STRUCT)
} gGpuSettings;

// #NOTE: Two sets of resources (one in flight and one being used on CPU)
const uint32_t gDataBufferCount = 2;

static bool gTogglePerformance = true;
static bool gToggleFXAA = true;

Renderer* pRenderer = NULL;

Queue*     pGraphicsQueue = NULL;
GpuCmdRing gGraphicsCmdRing = {};

SwapChain*    pSwapChain = NULL;
RenderTarget* pDepthBuffer = NULL;

RenderTarget* pLinearDepthBuffer = NULL;

Shader*   pLinearDepthResolveShader = NULL;
Pipeline* pLinearDepthResolvePipeline = NULL;

// Shader*             pLinearDepthCompShader = NULL;
// Pipeline*           pLinearDepthCompPipeline = NULL;
// RootSignature*      pLinearDepthCompRootSignature = NULL;

RenderTarget* pTerrainResultRT = NULL;
RenderTarget* pSkydomeResultRT = NULL;

Shader*   pFXAAShader = NULL;
Pipeline* pFXAAPipeline = NULL;

DescriptorSet* pExampleDescriptorSet = NULL;
RootSignature* pExampleRootSignature = NULL;
uint32_t       gExampleRootConstantIndex = 0;

Sampler* pBilinearClampSampler = NULL;

Semaphore* pImageAcquiredSemaphore = NULL;

ProfileToken gGpuProfileToken;
UIComponent* pMainGuiWindow = NULL;

Buffer* pTransmittanceBuffer = NULL;

static float2 LightDirection = float2(0.0f, 270.0f);
static float4 LightColorAndIntensity = float4(1.0f, 1.0f, 1.0f, 1.0f);

#define NEAR_CAMERA 50.0f
#define FAR_CAMERA  100000000.0f

static uint  gPrevFrameIndex = 0;
static bool  bSunMove = true;
static float SunMovingSpeed = 5.0f;

float mChildIndent = 25.0f;
float mHeightOffset = 20.0f;

//--------------------------------------------------------------------------------------------
// MESHES
//--------------------------------------------------------------------------------------------
typedef enum MeshResource
{
    MESH_MAT_BALL,
    MESH_CUBE,
    MESH_COUNT,
} MeshResource;

struct Vertex
{
    float3 mPos;
    float3 mNormal;
    float2 mUv;
};

struct FXAAINFO
{
    vec2  ScreenSize;
    float Use;
    float Time;
};

FXAAINFO gFXAAinfo;

uint32_t gFrameIndex = 0;

ICameraController* pCameraController = NULL;

VolumetricClouds gVolumetricClouds;
Terrain          gTerrain;
Sky              gSky;
SpaceObjects     gSpaceObjects;

FontDrawDesc gFrameTimeDraw;
FontDrawDesc gDefaultTextDrawDesc;
uint32_t     gFontID = 0;

const char*    pPipelineCacheName = "PipelineCache.cache";
PipelineCache* pPipelineCache = NULL;

static HiresTimer gTimer;

class RenderEphemeris: public IApp
{
public:
    bool Init()
    {
        initHiresTimer(&gTimer);

        // FILE PATHS
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_BINARIES, "CompiledShaders");
        fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG, RD_PIPELINE_CACHE, "PipelineCaches");
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_GPU_CONFIG, "GPUCfg");
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_TEXTURES, "Textures");
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_MESHES, "Meshes");
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_FONTS, "Fonts");
        fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SCRIPTS, "Scripts");
        fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG, RD_SCREENSHOTS, "Screenshots");
        fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG, RD_DEBUG, "Debug");

        CameraMotionParameters cmp{ 32000.0f, 120000.0f, 40000.0f };

        float h = 6000.0f;

        vec3 camPos{ 0.0f, h, -10.0f };
        vec3 lookAt{ 0.0f, h, 1.0f };

        pCameraController = initFpsCameraController(camPos, lookAt);
        pCameraController->setMotionParameters(cmp);

        ExtendedSettings extendedSettings = {};
        extendedSettings.mNumSettings = ESettings::Count;
        extendedSettings.pSettings = (uint32_t*)&gGpuSettings;
        extendedSettings.ppSettingNames = gSettingNames;

        RendererDesc settings;
        memset(&settings, 0, sizeof(settings));
        settings.pExtendedSettings = &extendedSettings;
        initRenderer(GetName(), &settings, &pRenderer);
        // check for init success
        if (!pRenderer)
            return false;

        if (gGpuSettings.mInsufficientBindlessEntries)
        {
            ShowUnsupportedMessage("Ephemeris does not run on this device. GPU does not support enough bindless texture entries");
            return false;
        }

        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_GRAPHICS;
        queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
        addQueue(pRenderer, &queueDesc, &pGraphicsQueue);

        GpuCmdRingDesc cmdRingDesc = {};
        cmdRingDesc.pQueue = pGraphicsQueue;
        cmdRingDesc.mPoolCount = gDataBufferCount;
        cmdRingDesc.mCmdPerPoolCount = 1;
        cmdRingDesc.mAddSyncPrimitives = true;
        addGpuCmdRing(pRenderer, &cmdRingDesc, &gGraphicsCmdRing);

        addSemaphore(pRenderer, &pImageAcquiredSemaphore);

        initResourceLoaderInterface(pRenderer);

        // Load fonts
        FontDesc font = {};
        font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
        fntDefineFonts(&font, 1, &gFontID);

        FontSystemDesc fontRenderDesc = {};
        fontRenderDesc.pRenderer = pRenderer;
        if (!initFontSystem(&fontRenderDesc))
            return false; // report?

        // Initialize Forge User Interface Rendering
        UserInterfaceDesc uiRenderDesc = {};
        uiRenderDesc.pRenderer = pRenderer;
        initUserInterface(&uiRenderDesc);

        PipelineCacheLoadDesc cacheDesc = {};
        cacheDesc.pFileName = pPipelineCacheName;
        loadPipelineCache(pRenderer, &cacheDesc, &pPipelineCache);

        InputSystemDesc inputDesc = {};
        inputDesc.pRenderer = pRenderer;
        inputDesc.pWindow = pWindow;
        if (!initInputSystem(&inputDesc))
            return false;

        UIComponentDesc UIComponentDesc = {};
        float           dpiScale[2];
        const uint32_t  monitorIdx = getActiveMonitorIdx();
        getMonitorDpiScale(monitorIdx, dpiScale);

        UIComponentDesc.mStartPosition = vec2(960.0f / dpiScale[0], 700.0f / dpiScale[1]);
        UIComponentDesc.mStartSize = vec2(300.0f / dpiScale[0], 250.0f / dpiScale[1]);
        UIComponentDesc.mFontID = 0;
        UIComponentDesc.mFontSize = 16.0f;
        uiCreateComponent("Global Settings", &UIComponentDesc, &pMainGuiWindow);

        // Initialize micro profiler and its UI.
        ProfilerDesc profiler = {};
        profiler.pRenderer = pRenderer;
        profiler.mWidthUI = mSettings.mWidth;
        profiler.mHeightUI = mSettings.mHeight;
        initProfiler(&profiler);

        gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "GpuProfiler");

#if defined(METAL)
        {
            float          dpiScale[2];
            const uint32_t monitorIdx = getActiveMonitorIdx();
            getMonitorDpiScale(monitorIdx, dpiScale);
            gFrameTimeDraw.mFontSize /= dpiScale[1];
            gFrameTimeDraw.mFontID = gFontID;
            gDefaultTextDrawDesc.mFontSize /= dpiScale[1];
        }
#endif
        // Add TransmittanceBuffer buffer
        BufferLoadDesc TransBufferDesc = {};
        TransBufferDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER | DESCRIPTOR_TYPE_BUFFER;
        TransBufferDesc.mDesc.mElementCount = 3;
        TransBufferDesc.mDesc.mStructStride = sizeof(float4);
        TransBufferDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
        TransBufferDesc.mDesc.mSize = TransBufferDesc.mDesc.mStructStride * TransBufferDesc.mDesc.mElementCount;
        TransBufferDesc.mDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        TransBufferDesc.mDesc.pName = "Transmittance Buffer";
        // TransBufferDesc.pData = gInitializeVal.data();
        TransBufferDesc.ppBuffer = &pTransmittanceBuffer;
        addResource(&TransBufferDesc, NULL);

        gTerrain.Initialize(pCameraController, gGpuProfileToken);
        gTerrain.Init(pRenderer, pPipelineCache);

        gSky.Initialize(pCameraController, gGpuProfileToken, pTransmittanceBuffer);
        gSky.Init(pRenderer, pPipelineCache);

        gVolumetricClouds.Initialize(pCameraController, pGraphicsQueue, gGpuProfileToken, pTransmittanceBuffer);
        gVolumetricClouds.Init(pRenderer, pPipelineCache);
        gTerrain.pWeatherMap = gVolumetricClouds.GetWeatherMap();

        gSpaceObjects.Initialize(pCameraController, gGpuProfileToken, pTransmittanceBuffer);
        gSpaceObjects.Init(pRenderer, pPipelineCache);

        SamplerDesc samplerClampDesc = { FILTER_LINEAR,
                                         FILTER_LINEAR,
                                         MIPMAP_MODE_LINEAR,
                                         ADDRESS_MODE_CLAMP_TO_EDGE,
                                         ADDRESS_MODE_CLAMP_TO_EDGE,
                                         ADDRESS_MODE_CLAMP_TO_EDGE };
        addSampler(pRenderer, &samplerClampDesc, &pBilinearClampSampler);

        CheckboxWidget checkbox;
        checkbox.pData = &gToggleFXAA;
        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "Enable FXAA", &checkbox, WIDGET_TYPE_CHECKBOX));

        SeparatorWidget separator;
        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "", &separator, WIDGET_TYPE_SEPARATOR));

        SliderFloatWidget sliderFloat;
        sliderFloat.pData = &LightDirection.x;
        sliderFloat.mMin = -180.0f;
        sliderFloat.mMax = 180.0f;
        sliderFloat.mStep = 0.001f;
        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "Light Azimuth", &sliderFloat, WIDGET_TYPE_SLIDER_FLOAT));

        sliderFloat.pData = &LightDirection.y;
        sliderFloat.mMin = 0.0f;
        sliderFloat.mMax = 360.0f;
        sliderFloat.mStep = 0.001f;
        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "Light Elevation", &sliderFloat, WIDGET_TYPE_SLIDER_FLOAT));

        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "", &separator, WIDGET_TYPE_SEPARATOR));

        SliderFloat4Widget sliderFloat4;
        sliderFloat4.pData = &LightColorAndIntensity;
        sliderFloat4.mMin = float4(0.0f);
        sliderFloat4.mMax = float4(10.0f);
        sliderFloat4.mStep = float4(0.01f);
        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "Light Color & Intensity", &sliderFloat4, WIDGET_TYPE_SLIDER_FLOAT4));

        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "", &separator, WIDGET_TYPE_SEPARATOR));

        checkbox.pData = &bSunMove;
        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "Automatic Sun Moving", &checkbox, WIDGET_TYPE_CHECKBOX));

        sliderFloat.pData = &SunMovingSpeed;
        sliderFloat.mMin = -100.0f;
        sliderFloat.mMax = 100.0f;
        sliderFloat.mStep = 0.01f;
        luaRegisterWidget(uiCreateComponentWidget(pMainGuiWindow, "Sun Moving Speed", &sliderFloat, WIDGET_TYPE_SLIDER_FLOAT));

        // App Actions
        InputActionDesc actionDesc = { DefaultInputActions::DUMP_PROFILE_DATA,
                                       [](InputActionContext* ctx)
                                       {
                                           dumpProfileData(((Renderer*)ctx->pUserData)->pName);
                                           return true;
                                       },
                                       pRenderer };
        addInputAction(&actionDesc);
        actionDesc = { DefaultInputActions::TOGGLE_FULLSCREEN,
                       [](InputActionContext* ctx)
                       {
                           WindowDesc* winDesc = ((IApp*)ctx->pUserData)->pWindow;
                           if (winDesc->fullScreen)
                               winDesc->borderlessWindow
                                   ? setBorderless(winDesc, getRectWidth(&winDesc->clientRect), getRectHeight(&winDesc->clientRect))
                                   : setWindowed(winDesc, getRectWidth(&winDesc->clientRect), getRectHeight(&winDesc->clientRect));
                           else
                               setFullscreen(winDesc);
                           return true;
                       },
                       this };
        addInputAction(&actionDesc);
        actionDesc = { DefaultInputActions::EXIT, [](InputActionContext* ctx)
                       {
                           requestShutdown();
                           return true;
                       } };
        addInputAction(&actionDesc);
        InputActionCallback onUIInput = [](InputActionContext* ctx)
        {
            if (ctx->mActionId > UISystemInputActions::UI_ACTION_START_ID_)
            {
                uiOnInput(ctx->mActionId, ctx->mBool, ctx->pPosition, &ctx->mFloat2);
            }
            return true;
        };

        typedef bool (*CameraInputHandler)(InputActionContext* ctx, DefaultInputActions::DefaultInputAction action);
        static CameraInputHandler onCameraInput = [](InputActionContext* ctx, DefaultInputActions::DefaultInputAction action)
        {
            if (*(ctx->pCaptured))
            {
                float2 delta = uiIsFocused() ? float2(0.f, 0.f) : ctx->mFloat2;
                switch (action)
                {
                case DefaultInputActions::ROTATE_CAMERA:
                    pCameraController->onRotate(delta);
                    break;
                case DefaultInputActions::TRANSLATE_CAMERA:
                    pCameraController->onMove(delta);
                    break;
                case DefaultInputActions::TRANSLATE_CAMERA_VERTICAL:
                    pCameraController->onMoveY(delta[0]);
                    break;
                default:
                    break;
                }
            }
            return true;
        };
        actionDesc = { DefaultInputActions::CAPTURE_INPUT,
                       [](InputActionContext* ctx)
                       {
                           setEnableCaptureInput(!uiIsFocused() && INPUT_ACTION_PHASE_CANCELED != ctx->mPhase);
                           return true;
                       },
                       NULL };
        addInputAction(&actionDesc);
        actionDesc = { DefaultInputActions::ROTATE_CAMERA,
                       [](InputActionContext* ctx) { return onCameraInput(ctx, DefaultInputActions::ROTATE_CAMERA); }, NULL };
        addInputAction(&actionDesc);
        actionDesc = { DefaultInputActions::TRANSLATE_CAMERA,
                       [](InputActionContext* ctx) { return onCameraInput(ctx, DefaultInputActions::TRANSLATE_CAMERA); }, NULL };
        addInputAction(&actionDesc);
        actionDesc = { DefaultInputActions::TRANSLATE_CAMERA_VERTICAL,
                       [](InputActionContext* ctx) { return onCameraInput(ctx, DefaultInputActions::TRANSLATE_CAMERA_VERTICAL); }, NULL };
        addInputAction(&actionDesc);
        actionDesc = { DefaultInputActions::RESET_CAMERA, [](InputActionContext* ctx)
                       {
                           if (!uiWantTextInput())
                               pCameraController->resetView();
                           return true;
                       } };
        addInputAction(&actionDesc);
        GlobalInputActionDesc globalInputActionDesc = { GlobalInputActionDesc::ANY_BUTTON_ACTION, onUIInput, this };
        setGlobalInputAction(&globalInputActionDesc);

        gFrameIndex = 0;

        return true;
    }

    void Exit()
    {
        exitCameraController(pCameraController);
        exitInputSystem();

        gFrameIndex = 0;
        gPrevFrameIndex = 0;
        gSpaceObjects.Exit();
        gVolumetricClouds.Exit();
        gSky.Exit();
        gTerrain.Exit();
        exitProfiler();

        removeResource(pTransmittanceBuffer);
        removeSampler(pRenderer, pBilinearClampSampler);

        exitUserInterface();

        exitFontSystem();

        removeSemaphore(pRenderer, pImageAcquiredSemaphore);
        removeGpuCmdRing(pRenderer, &gGraphicsCmdRing);

        removeQueue(pRenderer, pGraphicsQueue);

        PipelineCacheSaveDesc saveDesc;
        saveDesc.pFileName = pPipelineCacheName;
        savePipelineCache(pRenderer, pPipelineCache, &saveDesc);
        removePipelineCache(pRenderer, pPipelineCache);

        exitResourceLoaderInterface(pRenderer);

        exitRenderer(pRenderer);
        pRenderer = NULL;
    }

    bool Load(ReloadDesc* pReloadDesc)
    {
        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            addShaders();
            addRootSignatures();
            addDescriptorSets();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            // set frame index to 0
            gFrameIndex = 0;

            if (!addSwapChain())
                return false;

            if (!addDepthBuffer())
                return false;

            if (pReloadDesc->mType & RELOAD_TYPE_RESIZE)
            {
                gTerrain.Load(mSettings.mWidth, mSettings.mHeight);
                gSky.Load(mSettings.mWidth, mSettings.mHeight);
                gVolumetricClouds.Load(mSettings.mWidth, mSettings.mHeight);
                gSpaceObjects.Load(mSettings.mWidth, mSettings.mHeight);
            }

            gTerrain.InitializeWithLoad(pDepthBuffer);
            gSky.InitializeWithLoad(pDepthBuffer, pLinearDepthBuffer);
            gVolumetricClouds.InitializeWithLoad(pLinearDepthBuffer, pDepthBuffer);
            gSpaceObjects.InitializeWithLoad(pDepthBuffer, pLinearDepthBuffer, gVolumetricClouds.pSavePrevTexture,
                                             gSky.GetParticleVertexBuffer(), gSky.GetParticleInstanceBuffer(), gSky.GetParticleCount(),
                                             sizeof(float) * 6, sizeof(ParticleData));

            addRenderTargets();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            addPipelines();
        }

        waitForAllResourceLoads();

        prepareDescriptorSets();

        UserInterfaceLoadDesc uiLoad = {};
        uiLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        uiLoad.mHeight = mSettings.mHeight;
        uiLoad.mWidth = mSettings.mWidth;
        uiLoad.mLoadType = pReloadDesc->mType;
        loadUserInterface(&uiLoad);

        FontSystemLoadDesc fontLoad = {};
        fontLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        fontLoad.mHeight = mSettings.mHeight;
        fontLoad.mWidth = mSettings.mWidth;
        fontLoad.mLoadType = pReloadDesc->mType;
        loadFontSystem(&fontLoad);

        initScreenshotInterface(pRenderer, pGraphicsQueue);

        return true;
    }

    void Unload(ReloadDesc* pReloadDesc)
    {
        waitQueueIdle(pGraphicsQueue);

        unloadFontSystem(pReloadDesc->mType);
        unloadUserInterface(pReloadDesc->mType);

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            removePipelines();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);

            removeRenderTargets();
            gSky.Unload();
            gVolumetricClouds.Unload();
            gSpaceObjects.Unload();
        }

        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            removeDescriptorSets();
            removeRootSignatures();
            removeShaders();
        }

        exitScreenshotInterface();
    }

    void Update(float deltaTime)
    {
        updateInputSystem(deltaTime, mSettings.mWidth, mSettings.mHeight);
        /************************************************************************/
        // Input
        /************************************************************************/

        pCameraController->update(deltaTime);
        /************************************************************************/
        // Scene Update
        /************************************************************************/
        static float currentTime = 0.0f;
        currentTime += deltaTime * 1000.0f;

        if (bSunMove)
        {
            LightDirection.y += deltaTime * SunMovingSpeed;

            if (LightDirection.y < 0.0f)
                LightDirection.y += 360.0f;

            LightDirection.y = fmodf(LightDirection.y, 360.0f);
        }

        float Azimuth = (PI / 180.0f) * LightDirection.x;
        float Elevation = (PI / 180.0f) * (LightDirection.y - 180.0f);
        float cosElevation = cosf(Elevation);
        vec3  sunDirection = normalize(vec3(cosf(Azimuth) * cosElevation, sinf(Elevation), sinf(Azimuth) * cosElevation));

        gSky.Azimuth = Azimuth;
        gSky.Elevation = Elevation;
        gSky.LightDirection = v3ToF3(sunDirection);
        gSky.LightColorAndIntensity = LightColorAndIntensity;
        gSky.Update(deltaTime);

        gVolumetricClouds.LightDirection = v3ToF3(sunDirection);
        gVolumetricClouds.LightColorAndIntensity = LightColorAndIntensity;
        gVolumetricClouds.Update(deltaTime);

        // after gVolumetricClouds.Update because we read back data it computes
        gTerrain.IsEnabledShadow = true;
        gTerrain.volumetricCloudsShadowCB.SettingInfo00 =
            vec4(1.0, gVolumetricClouds.volumetricCloudsCB.m_DataPerLayer[0].CloudCoverage,
                 gVolumetricClouds.volumetricCloudsCB.m_DataPerLayer[0].WeatherTextureSize, 0.0);
        gTerrain.volumetricCloudsShadowCB.StandardPosition = gVolumetricClouds.volumetricCloudsCB.m_DataPerLayer[0].WindDirection;
        gTerrain.volumetricCloudsShadowCB.ShadowInfo = gVolumetricClouds.g_ShadowInfo;
        gTerrain.LightDirection = v3ToF3(sunDirection);
        gTerrain.LightColorAndIntensity = LightColorAndIntensity;
        gTerrain.SunColor = gSky.GetSunColor();
        gTerrain.Update(deltaTime);

        gSpaceObjects.Azimuth = Azimuth;
        gSpaceObjects.Elevation = Elevation;
        gSpaceObjects.LightDirection = v3ToF3(sunDirection);
        gSpaceObjects.LightColorAndIntensity = LightColorAndIntensity;
        gSpaceObjects.Update(deltaTime);

        gFXAAinfo.ScreenSize = vec2((float)mSettings.mWidth, (float)mSettings.mHeight);
        gFXAAinfo.Use = gToggleFXAA ? 1.0f : 0.0f;
        gFXAAinfo.Time = currentTime;
    }

    void Draw()
    {
        if (pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pGraphicsQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        uint32_t presentIndex = 0;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &presentIndex);

        // update camera with time
        // mat4 viewMat = pCameraController->getViewMatrix();

        // Stall if CPU is running "gDataBufferCount" frames ahead of GPU
        GpuCmdRingElement elem = getNextGpuCmdRingElement(&gGraphicsCmdRing, true, 1);
        FenceStatus       fenceStatus;
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);

        resetCmdPool(pRenderer, elem.pCmdPool);

        Cmd* cmd = elem.pCmds[0];
        beginCmd(cmd);

        cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);

        RenderTarget* pRenderTarget = pTerrainResultRT;

        ///////////////////////////////////////////////// Terrain ////////////////////////////////////////////////////

        gTerrain.gFrameIndex = gFrameIndex;
        gTerrain.Draw(cmd);

        struct CameraInfo
        {
            float nearPlane;
            float farPlane;
            float padding00;
            float padding01;
        };

        CameraInfo cameraInfo;
        cameraInfo.nearPlane = NEAR_CAMERA;
        cameraInfo.farPlane = FAR_CAMERA;
        ///////////////////////////////////////////////// Depth Linearization ////////////////////////////////////////////////////
        {
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Depth Linearization");

            RenderTargetBarrier barriersLinearDepth[] = { { pLinearDepthBuffer, RESOURCE_STATE_SHADER_RESOURCE,
                                                            RESOURCE_STATE_RENDER_TARGET } };

            cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriersLinearDepth);

            LoadActionsDesc loadActions = {};
            loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
            cmdBindRenderTargets(cmd, 1, &pLinearDepthBuffer, NULL, &loadActions, NULL, NULL, -1, -1);
            cmdSetViewport(cmd, 0.0f, 0.0f, (float)pLinearDepthBuffer->mWidth, (float)pLinearDepthBuffer->mHeight, 0.0f, 1.0f);
            cmdSetScissor(cmd, 0, 0, pLinearDepthBuffer->mWidth, pLinearDepthBuffer->mHeight);

            cmdBindPipeline(cmd, pLinearDepthResolvePipeline);
            cmdBindPushConstants(cmd, pExampleRootSignature, gExampleRootConstantIndex, &cameraInfo);
            cmdBindDescriptorSet(cmd, 0, pExampleDescriptorSet);
            cmdDraw(cmd, 3, 0);

            cmdBindRenderTargets(cmd, 0, NULL, 0, NULL, NULL, NULL, -1, -1);

            RenderTargetBarrier barriersLinearDepthEnd[] = { { pLinearDepthBuffer, RESOURCE_STATE_RENDER_TARGET,
                                                               RESOURCE_STATE_SHADER_RESOURCE } };

            cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriersLinearDepthEnd);

            cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
        }

        ///////////////////////////////////////////////// Sky ////////////////////////////////////////////////////

        gSky.gFrameIndex = gFrameIndex;
        gSky.Draw(cmd);

        ///////////////////////////////////////////////// Volumetric Clouds ////////////////////////////////////////////////////

        gVolumetricClouds.Update(gFrameIndex);
        gVolumetricClouds.Draw(cmd);

        ///////////////////////////////////////////////// Space Object ////////////////////////////////////////////////////

        gSpaceObjects.Draw(cmd);

        ///////////////////////////////////////////////// FXAA ////////////////////////////////////////////////////////////////

        {
            if (gToggleFXAA)
                cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "FXAA");
            else
                cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "PresentPipeline");

            pRenderTarget = pSwapChain->ppRenderTargets[presentIndex];

            RenderTargetBarrier barriers[] = {
                { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
            };

            cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

            LoadActionsDesc loadActions = {};
            loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
            loadActions.mClearColorValues[0].r = 0.0f;
            loadActions.mClearColorValues[0].g = 0.0f;
            loadActions.mClearColorValues[0].b = 0.0f;
            loadActions.mClearColorValues[0].a = 0.0f;
            loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;

            cmdBindRenderTargets(cmd, 1, &pRenderTarget, NULL, &loadActions, NULL, NULL, -1, -1);
            cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 0.0f, 1.0f);
            cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

            cmdBindPipeline(cmd, pFXAAPipeline);

            cmdBindDescriptorSet(cmd, 1, pExampleDescriptorSet);
            cmdBindPushConstants(cmd, pExampleRootSignature, gExampleRootConstantIndex, &gFXAAinfo);

            cmdDraw(cmd, 3, 0);

            cmdBindRenderTargets(cmd, 0, NULL, 0, NULL, NULL, NULL, -1, -1);

            cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
        }

        // UI
        {
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw UI");

            pRenderTarget = pSwapChain->ppRenderTargets[presentIndex];

            LoadActionsDesc loadActions = {};
            loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
            cmdBindRenderTargets(cmd, 1, &pRenderTarget, NULL, &loadActions, NULL, NULL, -1, -1);
            cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 0.0f, 1.0f);
            cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

            getHiresTimerUSec(&gTimer, true);

            if (gTogglePerformance)
            {
                gFrameTimeDraw.mFontColor = 0xff00ffff;
                gFrameTimeDraw.mFontSize = 18.0f;
                gFrameTimeDraw.mFontID = gFontID;
                cmdDrawCpuProfile(cmd, float2(8.0f, 15.0f), &gFrameTimeDraw);
                cmdDrawGpuProfile(cmd, float2(8.0f, 100.0f), gGpuProfileToken, &gFrameTimeDraw);
            }

            cmdDrawUserInterface(cmd);

            cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

            RenderTargetBarrier barriers[] = { { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT } };
            cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

            cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
        }

        cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
        endCmd(cmd);

        FlushResourceUpdateDesc flushUpdateDesc = {};
        flushUpdateDesc.mNodeIndex = 0;
        flushResourceUpdates(&flushUpdateDesc);
        Semaphore* waitSemaphores[2] = { flushUpdateDesc.pOutSubmittedSemaphore, pImageAcquiredSemaphore };

        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.mSignalSemaphoreCount = 1;
        submitDesc.mWaitSemaphoreCount = TF_ARRAY_COUNT(waitSemaphores);
        submitDesc.ppCmds = &cmd;
        submitDesc.ppSignalSemaphores = &elem.pSemaphore;
        submitDesc.ppWaitSemaphores = waitSemaphores;
        submitDesc.pSignalFence = elem.pFence;
        queueSubmit(pGraphicsQueue, &submitDesc);
        QueuePresentDesc presentDesc = {};
        presentDesc.mIndex = presentIndex;
        presentDesc.mWaitSemaphoreCount = 1;
        presentDesc.ppWaitSemaphores = &elem.pSemaphore;
        presentDesc.pSwapChain = pSwapChain;
        presentDesc.mSubmitDone = true;
        queuePresent(pGraphicsQueue, &presentDesc);

        flipProfiler();

        // for next frame
        gPrevFrameIndex = gFrameIndex;
        gFrameIndex = (gFrameIndex + 1) % gDataBufferCount;

        if (gVolumetricClouds.AfterSubmit(gPrevFrameIndex))
        {
            waitQueueIdle(pGraphicsQueue);

            gSpaceObjects.Unload();
            gVolumetricClouds.Unload();
            gVolumetricClouds.removeRenderTargets();

            gVolumetricClouds.InitializeWithLoad(pLinearDepthBuffer, pDepthBuffer);
            gVolumetricClouds.Load(mSettings.mWidth, mSettings.mHeight);
            gVolumetricClouds.addRenderTargets();

            gSpaceObjects.InitializeWithLoad(pDepthBuffer, pLinearDepthBuffer, gVolumetricClouds.pSavePrevTexture,
                                             gSky.GetParticleVertexBuffer(), gSky.GetParticleInstanceBuffer(), gSky.GetParticleCount(),
                                             sizeof(float) * 6, sizeof(ParticleData));
            gSpaceObjects.Load(mSettings.mWidth, mSettings.mHeight);

            gTerrain.pWeatherMap = gVolumetricClouds.GetWeatherMap();

            RenderTarget* ppVolumetricCloudsUsedRTs[2] = { gSky.pSkyRenderTarget, gSky.pSkyRenderTarget };
            gVolumetricClouds.prepareDescriptorSets(ppVolumetricCloudsUsedRTs, 2);
            gSpaceObjects.prepareDescriptorSets(&gSky.pSkyRenderTarget);
        }
    }

    const char* GetName() { return "Ephemeris"; }

    bool addSwapChain()
    {
        SwapChainDesc swapChainDesc = {};
        swapChainDesc.mWindowHandle = pWindow->handle;
        swapChainDesc.mPresentQueueCount = 1;
        swapChainDesc.ppPresentQueues = &pGraphicsQueue;
        swapChainDesc.mWidth = mSettings.mWidth;
        swapChainDesc.mHeight = mSettings.mHeight;
        swapChainDesc.mImageCount = getRecommendedSwapchainImageCount(pRenderer, &pWindow->handle);
        swapChainDesc.mColorFormat = getSupportedSwapchainFormat(pRenderer, &swapChainDesc, COLOR_SPACE_SDR_SRGB);
        swapChainDesc.mColorSpace = COLOR_SPACE_SDR_SRGB;
        swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
        ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

        return pSwapChain != NULL;
    }

    void addDescriptorSets()
    {
        gTerrain.addDescriptorSets();
        gSky.addDescriptorSets();
        gVolumetricClouds.addDescriptorSets();
        gSpaceObjects.addDescriptorSets();

        DescriptorSetDesc setDesc = { pExampleRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 2 };
        addDescriptorSet(pRenderer, &setDesc, &pExampleDescriptorSet);
    }

    void removeDescriptorSets()
    {
        gSpaceObjects.removeDescriptorSets();
        gVolumetricClouds.removeDescriptorSets();
        gSky.removeDescriptorSets();
        gTerrain.removeDescriptorSets();

        removeDescriptorSet(pRenderer, pExampleDescriptorSet);
    }

    void addRootSignatures()
    {
        gTerrain.addRootSignatures();
        gSky.addRootSignatures();
        gVolumetricClouds.addRootSignatures();
        gSpaceObjects.addRootSignatures();

        RootSignatureDesc rootDesc;
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////
        /*rootDesc = {};
        rootDesc = { 0 };
        rootDesc.mShaderCount = 1;
        rootDesc.ppShaders = &pLinearDepthCompShader;

        addRootSignature(pRenderer, &rootDesc, &pLinearDepthCompRootSignature);*/
        ///////////////////////////////////////////////////////////////////////////////////////////////////////////

        const char* pStaticSamplerNames[] = { "g_LinearClamp" };
        Sampler*    pStaticSamplers[] = { pBilinearClampSampler };
        Shader*     shaders[] = { pFXAAShader, pLinearDepthResolveShader };
        rootDesc = {};
        rootDesc.mShaderCount = 2;
        rootDesc.ppStaticSamplerNames = pStaticSamplerNames;
        rootDesc.ppStaticSamplers = pStaticSamplers;
        rootDesc.ppShaders = shaders;
        rootDesc.mStaticSamplerCount = 1;
        addRootSignature(pRenderer, &rootDesc, &pExampleRootSignature);
        gExampleRootConstantIndex = getDescriptorIndexFromName(pExampleRootSignature, "ExampleRootConstant");
    }

    void removeRootSignatures()
    {
        gSpaceObjects.removeRootSignatures();
        gVolumetricClouds.removeRootSignatures();
        gSky.removeRootSignatures();
        gTerrain.removeRootSignatures();

        // removeRootSignature(pRenderer, pLinearDepthCompRootSignature);
        removeRootSignature(pRenderer, pExampleRootSignature);
    }

    void addShaders()
    {
        gTerrain.addShaders();
        gSky.addShaders();
        gVolumetricClouds.addShaders();
        gSpaceObjects.addShaders();

        ShaderLoadDesc FXAAShader = {};
        FXAAShader.mStages[0].pFileName = "Triangular.vert";
        FXAAShader.mStages[1].pFileName = "FXAA.frag";
        addShader(pRenderer, &FXAAShader, &pFXAAShader);

        ShaderLoadDesc depthLinearizationResolveShader = {};
        depthLinearizationResolveShader.mStages[0].pFileName = "Triangular.vert";
        depthLinearizationResolveShader.mStages[1].pFileName = "DepthLinearization.frag";
        addShader(pRenderer, &depthLinearizationResolveShader, &pLinearDepthResolveShader);

        /*ShaderLoadDesc depthLinearizationShader = {};
        depthLinearizationShader.mStages[0] = { "depthLinearization.comp", NULL, 0 };
        addShader(pRenderer, &depthLinearizationShader, &pLinearDepthCompShader);
        */
    }

    void removeShaders()
    {
        gSpaceObjects.removeShaders();
        gVolumetricClouds.removeShaders();
        gSky.removeShaders();
        gTerrain.removeShaders();

        removeShader(pRenderer, pFXAAShader);
        // removeShader(pRenderer, pLinearDepthCompShader);
        removeShader(pRenderer, pLinearDepthResolveShader);
    }

    void addPipelines()
    {
        gTerrain.addPipelines();
        gSky.addPipelines();
        gVolumetricClouds.addPipelines();
        gSpaceObjects.addPipelines();

        RasterizerStateDesc rasterizerStateDesc = {};
        rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

        {
            PipelineDesc desc = {};
            desc.pCache = pPipelineCache;
            desc.mType = PIPELINE_TYPE_GRAPHICS;
            GraphicsPipelineDesc& pipelineSettings = desc.mGraphicsDesc;
            pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            pipelineSettings.mRenderTargetCount = 1;
            pipelineSettings.pDepthState = NULL;
            pipelineSettings.pBlendState = NULL;
            pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
            pipelineSettings.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
            pipelineSettings.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
            pipelineSettings.pRootSignature = pExampleRootSignature;
            pipelineSettings.pRasterizerState = &rasterizerStateDesc;
            pipelineSettings.pShaderProgram = pFXAAShader;
            addPipeline(pRenderer, &desc, &pFXAAPipeline);
        }

        PipelineDesc LinearDepthResolvePipelineDesc = {};
        LinearDepthResolvePipelineDesc.pCache = pPipelineCache;
        {
            LinearDepthResolvePipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;
            GraphicsPipelineDesc& pipelineSettings = LinearDepthResolvePipelineDesc.mGraphicsDesc;

            pipelineSettings = { 0 };
            pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            pipelineSettings.mRenderTargetCount = 1;
            pipelineSettings.pDepthState = NULL;
            pipelineSettings.pColorFormats = &pLinearDepthBuffer->mFormat;
            pipelineSettings.mSampleCount = pLinearDepthBuffer->mSampleCount;
            pipelineSettings.mSampleQuality = pLinearDepthBuffer->mSampleQuality;
            pipelineSettings.pRootSignature = pExampleRootSignature;
            pipelineSettings.pShaderProgram = pLinearDepthResolveShader;
            pipelineSettings.pRasterizerState = &rasterizerStateDesc;

            addPipeline(pRenderer, &LinearDepthResolvePipelineDesc, &pLinearDepthResolvePipeline);
        }

        /*PipelineDesc LinearDepthCompPipelineDesc = {};
        LinearDepthCompPipelineDesc.pCache = pPipelineCache;
        {
            LinearDepthCompPipelineDesc.mType = PIPELINE_TYPE_COMPUTE;
            ComputePipelineDesc &comPipelineSettings = LinearDepthCompPipelineDesc.mComputeDesc;

            comPipelineSettings = { 0 };
            comPipelineSettings.pShaderProgram = pLinearDepthCompShader;
            comPipelineSettings.pRootSignature = pLinearDepthCompRootSignature;
            addPipeline(pRenderer, &LinearDepthCompPipelineDesc, &pLinearDepthCompPipeline);
        }*/
    }

    void removePipelines()
    {
        gSpaceObjects.removePipelines();
        gVolumetricClouds.removePipelines();
        gSky.removePipelines();
        gTerrain.removePipelines();

        removePipeline(pRenderer, pLinearDepthResolvePipeline);
        // removePipeline(pRenderer, pLinearDepthCompPipeline);
        removePipeline(pRenderer, pFXAAPipeline);
    }

    void prepareDescriptorSets()
    {
        gTerrain.prepareDescriptorSets();
        gSky.prepareDescriptorSets(&gTerrain.pTerrainRT);
        RenderTarget* ppVolumetricCloudsUsedRTs[2] = { gSky.pSkyRenderTarget, gSky.pSkyRenderTarget };
        gVolumetricClouds.prepareDescriptorSets(ppVolumetricCloudsUsedRTs, 2);
        gSpaceObjects.prepareDescriptorSets(&gSky.pSkyRenderTarget);

        DescriptorData LinearDepthpparams[1] = {};
        LinearDepthpparams[0].pName = "SrcTexture";
        LinearDepthpparams[0].ppTextures = &pDepthBuffer->pTexture;
        updateDescriptorSet(pRenderer, 0, pExampleDescriptorSet, 1, LinearDepthpparams);

        DescriptorData Presentpparams[1] = {};
        Presentpparams[0].pName = "SrcTexture";
        Presentpparams[0].ppTextures = &(gSky.pSkyRenderTarget->pTexture);
        updateDescriptorSet(pRenderer, 1, pExampleDescriptorSet, 1, Presentpparams);

        /*
                DescriptorData FXAApparams[1] = {};
                FXAApparams[0].pName = "SrcTexture";
                FXAApparams[0].ppTextures = &(gSky.pSkyRenderTarget->pTexture);
                updateDescriptorSet(pRenderer, 2, pExampleDescriptorSet, 1, FXAApparams);
        */
    }

    bool addDepthBuffer()
    {
        // Add depth buffer
        RenderTargetDesc depthRT = {};
        depthRT.mArraySize = 1;
        depthRT.mClearValue.depth = 1.0f;
        depthRT.mClearValue.stencil = 0;
        depthRT.mDepth = 1;
        depthRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
        depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
        depthRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        depthRT.mHeight = mSettings.mHeight;
        depthRT.mSampleCount = SAMPLE_COUNT_1;
        depthRT.mSampleQuality = 0;
        depthRT.mWidth = mSettings.mWidth;
        addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);

        // Add linear depth Texture
        depthRT.mClearValue = {};
        depthRT.mFormat = TinyImageFormat_R16_SFLOAT;
        depthRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        depthRT.mWidth = mSettings.mWidth & (~63);
        depthRT.mHeight = mSettings.mHeight & (~63);
        addRenderTarget(pRenderer, &depthRT, &pLinearDepthBuffer);

        return pDepthBuffer != NULL && pLinearDepthBuffer != NULL;
    }

    void addRenderTargets()
    {
        gTerrain.addRenderTargets();
        gSky.addRenderTargets();
        gVolumetricClouds.addRenderTargets();

        RenderTargetDesc resultRT = {};
        resultRT.mArraySize = 1;
        resultRT.mDepth = 1;
        resultRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
        resultRT.mFormat = pSwapChain->mFormat;
        resultRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        resultRT.mSampleCount = SAMPLE_COUNT_1;
        resultRT.mSampleQuality = 0;

        resultRT.mWidth = mSettings.mWidth;
        resultRT.mHeight = mSettings.mHeight;

        addRenderTarget(pRenderer, &resultRT, &pSkydomeResultRT);
    }

    void removeRenderTargets()
    {
        gVolumetricClouds.removeRenderTargets();
        gSky.removeRenderTargets();
        gTerrain.removeRenderTargets();

        removeRenderTarget(pRenderer, pDepthBuffer);
        removeRenderTarget(pRenderer, pLinearDepthBuffer);
        removeRenderTarget(pRenderer, pSkydomeResultRT);
    }

    void RecenterCameraView(float maxDistance, const vec3& lookAt = vec3(0))
    {
        vec3 p = pCameraController->getViewPosition();
        vec3 d = p - lookAt;

        float lenSqr = lengthSqr(d);
        if (lenSqr > (maxDistance * maxDistance))
        {
            d *= (maxDistance / sqrtf(lenSqr));
        }

        p = d + lookAt;
        pCameraController->moveTo(p);
        pCameraController->lookAt(lookAt);
    }
};

DEFINE_APPLICATION_MAIN(RenderEphemeris)
