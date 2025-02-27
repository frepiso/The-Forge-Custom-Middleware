/*
 * Copyright (c) 2017-2024 The Forge Interactive Inc.
 *
 * This is a part of Aura.
 * This file(code) is licensed under a Creative Commons Attribution-NonCommercial 4.0 International License
 * (https://creativecommons.org/licenses/by-nc/4.0/legalcode) Based on a work at https://github.com/ConfettiFX/The-Forge. You can not use
 * this code for commercial purposes.
 *
 */

#pragma once

#include "../Interfaces/IAuraTaskManager.h"

#include "../Config/AuraParams.h"
#include "../Math/AuraVector.h"

#include "LightPropagationRenderer.h"

#ifdef XBOX
#include <DirectXPackedVector.h>
#endif

#define NO_FSL_DEFINITIONS
#include "../Shaders/FSL/lpvCommon.h"

#include "LightPropagationCPUContext.h"
#include "LightPropagationCascade.h"

// #include "SSGI/SSGIHandler.h"

//	TODO: Igor: replace this with the interface header include.
// #include "../Include/AuraParams.h"

namespace aura
{
static const uint32_t MAX_FRAMES = 2U;

enum CascadeOptions
{
    CASCADE_NOT_MOVING = 0x01,
};

struct Box
{
    vec3 vMin;
    vec3 vMax;
};

typedef struct LightPropagationCascadeDesc
{
    float    mGridSpan;
    float    mGridIntensity;
    uint32_t mFlags;
} LightPropagationCascadeDesc;

typedef struct Aura
{
    Renderer*                    pRenderer;
    LightPropagationVolumeParams mParams;
    CPUPropagationParams         mCPUParams;

    //	Capture current, propagate current+1, apply current+2
#ifdef ENABLE_CPU_PROPAGATION
    int32_t                      mCPUPropagationCurrentContext;
    LightPropagationCPUContext** m_CPUContexts;
    bool                         bUseCPUPropagationPreviousFrame; // Used to detect if switching between CPU and GPU propagation.
    // The CPU propagation runs behind the GPU by this many frames so that data is always available.
    uint32_t                     mInFlightFrameCount;
#endif
    int32_t mGPUPropagationCurrentGrid;

    RenderTarget*             pWorkingGrids[6];
    uint32_t                  mCascadeCount;
    LightPropagationCascade** pCascades;

    uint32_t mFrameIdx;

    Shader* pShaderDebugDrawVolume;
    Shader* pShaderDebugDrawOccluders;
    Shader* pShaderLPVVisualize;
    Shader* pShaderAreaLight;
    Shader* pShaderhInjectOccluder;
    Shader* pShaderInjectOccluderCube;
    Shader* pShaderCopyOccluder;
    Shader* pShaderInjectRSMLight;
    Shader* pShaderInjectRSMLightCube;
#if !defined(ORBIS) // causes error : private field 'NAME' is not used
    Shader* pShaderLightPropagate;
#endif
    Shader* pShaderLightPropagate1[2];
    Shader* pShaderLightPropagateN[2];
    Shader* pShaderLightCopy;

    Pipeline* pPipelineInjectRSMLight;
    Pipeline* pPipelineLightPropagate1[2];
    Pipeline* pPipelineLightPropagateN[2];
    Pipeline* pPipelineLightCopy;
    Pipeline* pPipelineVisualizeLPV;

    RootSignature* pRootSignatureInjectRSMLight;
    RootSignature* pRootSignatureLightPropagate1;
    RootSignature* pRootSignatureLightPropagateN;
    RootSignature* pRootSignatureLightCopy;
    RootSignature* pRootSignatureVisualizeLPV;
    uint32_t       mPropagation1RootConstantIndex;
    uint32_t       mPropagationNRootConstantIndex;

    DescriptorSet* pDescriptorSetInjectRSMLight;
    DescriptorSet* pDescriptorSetLightPropagate1;
    DescriptorSet* pDescriptorSetLightPropagateN;
    DescriptorSet* pDescriptorSetLightCopy;
    DescriptorSet* pDescriptorSetVisualizeLPV;

    Buffer** pUniformBufferInjectRSM[MAX_FRAMES];
    Buffer*  pUniformBufferVisualizationData[MAX_FRAMES];

    Sampler* pSamplerLinearBorder;
    Sampler* pSamplerPointBorder;
} Aura;

void initAura(Renderer* pRenderer, uint32_t rtWidth, uint32_t rtHeight, LightPropagationVolumeParams params, uint32_t inFlightFrameCount,
              uint32_t cascadeCount, LightPropagationCascadeDesc* pCascades, Aura** ppAura);
void loadCPUPropagationResources(Renderer* pRenderer, Aura* pAura);
void exitAura(Renderer* pRenderer, ITaskManager* pTaskManager, Aura* pAura);

void     setCascadeCenter(Aura* pAura, uint32_t Cascade, const vec3& center);
void     getGridBounds(Aura* pAura, const mat4& worldToLocal, Box* bounds);
uint32_t getCascadesToUpdateMask(Aura* pAura);

void beginFrame(Renderer* pRenderer, Aura* pAura, const vec3& camPos, const vec3& camDir);
void endFrame(Renderer* pRenderer, Aura* pAura);
void mapAsyncResources(Renderer* pRenderer);

void addDescriptorSets();
void removeDescriptorSets();
void addRootSignatures();
void removeRootSignatures();
void addShaders();
void removeShaders();
void addPipelines(PipelineCache* pCache, TinyImageFormat visualizeFormat, TinyImageFormat visualizeDepthFormat, SampleCount sampleCount,
                  uint32_t sampleQuality);
void removePipelines();
void prepareDescriptorSets();

void injectRSM(Cmd* pCmd, Renderer* pRenderer, Aura* pAura, uint32_t iVolume, const mat4& invVP, const vec3& camDir, uint32_t rtWidth,
               uint32_t rtHeight, float viewAreaForUnitDepth, Texture* baseRT, Texture* normalRT, Texture* depthRT);
// CPU propagation
void captureLight(Cmd* pCmd, Aura* pAura);

void propagateLight(Cmd* pCmd, Renderer* pRenderer, ITaskManager* pTaskManager, Aura* pAura);
void applyLight(Cmd* pCmd, Renderer* pRenderer, Aura* pAura, const mat4& invVP, const vec3& camPos, Texture* normalRT, Texture* depthRT,
                Texture* ambientOcclusionRT);
void getLightApplyData(Aura* pAura, const mat4& invVP, const vec3& camPos, LightApplyData* data);

void drawLpvVisualization(Cmd* cmd, Renderer* pRenderer, Aura* pAura, RenderTarget* renderTarget, RenderTarget* depthRenderTarget,
                          const mat4& projection, const mat4& view, const mat4& inverseView, int cascadeIndex, float probeSize);

/************************************************************************/
/************************************************************************/
} // namespace aura
