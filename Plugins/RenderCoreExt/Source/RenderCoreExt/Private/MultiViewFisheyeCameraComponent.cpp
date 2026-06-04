/*
 * Copyright (c) 2026 mengrongye.
 *
 * Author: mengrongye
 * Contact: mengrongye@gmail.com
 * Project: OmniCaptureUE / RenderCoreExt
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "MultiViewFisheyeCameraComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "ImageWriteBlueprintLibrary.h" // Add this include
#include "JsonObjectConverter.h"
#include "HAL/IConsoleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"

UMultiViewFisheyeCameraComponent::UMultiViewFisheyeCameraComponent()
{
    // 默认参数 (对齐 User 需求，防止 Parsing 失败导致回退到 45/120)
    FisheyeParameters.HorizontalFOV = 180.0f;
    FisheyeParameters.VerticalFOV = 150.0f; // 之前是 120
    FisheyeParameters.AngleStep = 45;    // 之前是 45
    
    // 默认 FOV 设为 90 (单个 View)
    
    // 默认 FOV 设为 90 (单个 View)
    FOVAngle = 90.0f; 
}

void UMultiViewFisheyeCameraComponent::BeginPlay()
{
    Super::BeginPlay();
    
    // 强制开启 Lumen SceneCapture 支持 (全局)
    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.SurfaceCache.CaptureLighting")))
    {
        CVar->Set(1, ECVF_SetByCode);
    }
    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.UpdateSceneCaptures")))
    {
        CVar->Set(1, ECVF_SetByCode);
    }
    
    // 增加 VSM 每个像素的最大灯光数，防止阴影溢出警告
    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel")))
    {
        CVar->Set(32, ECVF_SetByCode); // 默认通常较低，增加到 32 以支持复杂光照场景
    }
    
    // 初始构建
    RebuildMultiViewCaptures();
}

void UMultiViewFisheyeCameraComponent::ConfigureCamera(const FFisheyeParams& Params)
{
    // 1. 调用父类配置基础参数
    Super::ConfigureCamera(Params);
    
    UE_LOG(LogTemp, Warning, TEXT("[MultiView] ConfigureCamera: Angle=%.2f, HFOV=%.2f, VFOV=%.2f. Current: Angle=%.2f"), 
        Params.AngleStep, Params.HorizontalFOV, Params.VerticalFOV, FisheyeParameters.AngleStep);

    if (FisheyeDepthPostProcessMaterial)
    {
        UE_LOG(LogTemp, Warning, TEXT("[MultiView] Depth Material Loaded Successfully: %s"), *FisheyeDepthPostProcessMaterial->GetName());
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[MultiView] Depth Material is NULL! Check path in Python command."));
    }
    
    // 2. 检查参数是否变化
    bool bNeedRebuild = false;
    // Check if parameters changed
    if (!FMath::IsNearlyEqual(FisheyeParameters.HorizontalFOV, Params.HorizontalFOV, 0.01f)) bNeedRebuild = true;
    if (!FMath::IsNearlyEqual(FisheyeParameters.VerticalFOV, Params.VerticalFOV, 0.01f)) bNeedRebuild = true;
    if (!FMath::IsNearlyEqual(FisheyeParameters.AngleStep, Params.AngleStep, 0.01f)) bNeedRebuild = true;

    FisheyeParameters.AngleStep = Params.AngleStep;
    FisheyeParameters.HorizontalFOV = Params.HorizontalFOV;
    FisheyeParameters.VerticalFOV = Params.VerticalFOV;

    // 3. 如果需要，重建所有子组件
    if (bNeedRebuild)
    {
        RebuildMultiViewCaptures();
    }
    
    // 4. 同步通用设置
    TArray<USceneCaptureComponent2D*> AllCaptures;
    if (FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial)
    {
        if (SharedCaptureComp) AllCaptures.Add(SharedCaptureComp);
    }
    else
    {
        for (auto& Probe : ProbePool) AllCaptures.Add(Probe.CaptureComp);
    }

    for (USceneCaptureComponent2D* CapComp : AllCaptures)
    {
        // --- 核心修复：Lumen & GI 强制开启 ---
        CapComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR; 
        CapComp->bAlwaysPersistRenderingState = true;
        CapComp->bCaptureEveryFrame = false;

        // 1. ShowFlags 全开
        CapComp->ShowFlags = this->ShowFlags; 
        CapComp->ShowFlags.SetLumenGlobalIllumination(true);
        CapComp->ShowFlags.SetLumenReflections(true);
        CapComp->ShowFlags.SetGlobalIllumination(true);
        CapComp->ShowFlags.SetIndirectLightingCache(true);
        CapComp->ShowFlags.SetLighting(true);
        CapComp->ShowFlags.SetSkyLighting(true);
        CapComp->ShowFlags.SetPostProcessing(true);
        
        // --- 修复拼接痕迹 ---
        CapComp->ShowFlags.SetScreenSpaceAO(false);
        CapComp->ShowFlags.SetContactShadows(false);
        CapComp->ShowFlags.SetScreenSpaceReflections(false);
        
        // --- 动态阴影开关 ---
        CapComp->ShowFlags.SetDynamicShadows(Params.bEnableShadows);
        
        // 2. PostProcess 强制 Lumen
        CapComp->PostProcessSettings = this->PostProcessSettings; 
        CapComp->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
        CapComp->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
        CapComp->PostProcessSettings.bOverride_ReflectionMethod = true;
        CapComp->PostProcessSettings.ReflectionMethod = EReflectionMethod::Lumen;
        
        // --- 关键修复：禁用 Lumen 屏幕空间追踪 (Screen Traces) ---
        CapComp->PostProcessSettings.bOverride_LumenFinalGatherScreenTraces = true;
        CapComp->PostProcessSettings.LumenFinalGatherScreenTraces = false;
        CapComp->PostProcessSettings.bOverride_LumenReflectionsScreenTraces = true;
        CapComp->PostProcessSettings.LumenReflectionsScreenTraces = false;

        CapComp->PostProcessSettings.bOverride_LumenSceneDetail = true;
        CapComp->PostProcessSettings.LumenSceneDetail = 1.0f;
        CapComp->PostProcessSettings.bOverride_LumenSceneViewDistance = true;
        CapComp->PostProcessSettings.LumenSceneViewDistance = 20000.0f;
        
        // Lumen 设置优化
        CapComp->PostProcessSettings.bOverride_LumenSceneLightingQuality = true;
        CapComp->PostProcessSettings.LumenSceneLightingQuality = 1.0f;
        
        // 3. 强制固定曝光
        CapComp->PostProcessSettings.bOverride_AutoExposureMethod = true;
        CapComp->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        CapComp->PostProcessSettings.bOverride_AutoExposureBias = true;
        CapComp->PostProcessSettings.AutoExposureBias = Params.ExposureBias;

        // Tone Curve / Gamma
        CapComp->PostProcessSettings.bOverride_ToneCurveAmount = true;
        CapComp->PostProcessSettings.ToneCurveAmount = Params.bEnableToneCurve ? 1.0f : 0.0f;

        // 4. 反走样 (Anti-Aliasing)
        CapComp->ShowFlags.SetTemporalAA(true); 
        
        // 5. 分辨率初始化
        float Multiplier = FMath::Max(1.0f, Params.SupersamplingMultiplier);
        int32 RTWidth = FMath::RoundToInt(Params.Resolution.X * Multiplier);
        int32 RTHeight = FMath::RoundToInt(Params.Resolution.Y * Multiplier);
        
        if (CapComp->TextureTarget)
        {
            CapComp->TextureTarget->InitCustomFormat(RTWidth, RTHeight, PF_B8G8R8A8, false);
            CapComp->TextureTarget->UpdateResourceImmediate(true);
        }
        CapComp->FOVAngle = 90.0f; 
    }
}

void UMultiViewFisheyeCameraComponent::SetupProbe(FMultiViewProbe& OutProbe, int32 Index)
{
    FString CompName = FString::Printf(TEXT("MultiView_Probe_%d"), Index);
    OutProbe.CaptureComp = NewObject<USceneCaptureComponent2D>(this, *CompName);
    OutProbe.CaptureComp->RegisterComponent();
    OutProbe.CaptureComp->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
    
    OutProbe.CaptureComp->TextureTarget = NewObject<UTextureRenderTarget2D>(OutProbe.CaptureComp);
    
    // 初始化分辨率 (ConfigureCamera 会进一步更新)
    OutProbe.CaptureComp->TextureTarget->InitCustomFormat(1024, 1024, PF_B8G8R8A8, false);
    OutProbe.CaptureComp->TextureTarget->UpdateResourceImmediate(true);
    
    OutProbe.CaptureComp->bCaptureEveryFrame = false;
    OutProbe.CaptureComp->bCaptureOnMovement = false;
    OutProbe.CaptureComp->bAlwaysPersistRenderingState = true;
    OutProbe.CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;

    OutProbe.DepthRT = NewObject<UTextureRenderTarget2D>(this);
    OutProbe.DepthRT->InitCustomFormat(1024, 1024, PF_A32B32G32R32F, false);
    OutProbe.DepthRT->UpdateResourceImmediate(true);

    OutProbe.RGBRT = NewObject<UTextureRenderTarget2D>(this);
    OutProbe.RGBRT->InitCustomFormat(1024, 1024, PF_B8G8R8A8, false);
    OutProbe.RGBRT->UpdateResourceImmediate(true);
}

void UMultiViewFisheyeCameraComponent::RebuildMultiViewCaptures()
{
    // 清理旧视图元数据
    ViewsMetadata.Empty();

    if (FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial)
    {
        // 串行模式：只使用 SharedCaptureComp
        if (!SharedCaptureComp)
        {
            FMultiViewProbe TempProbe;
            SetupProbe(TempProbe, 0);
            SharedCaptureComp = TempProbe.CaptureComp;
            SharedDepthRT = TempProbe.DepthRT;
        }
        // 清理并行池
        ProbePool.Empty();
    }
    else
    {
        // 并行模式：初始化探针池
        int32 NumProbes = FMath::Max(1, FisheyeParameters.NumParallelProbes);
        
        // 如果池大小变化，重新构建
        if (ProbePool.Num() != NumProbes)
        {
            ProbePool.Empty();
            for (int32 i = 0; i < NumProbes; i++)
            {
                FMultiViewProbe NewProbe;
                SetupProbe(NewProbe, i);
                ProbePool.Add(NewProbe);
            }
        }
        
        // 兼容旧引用
        if (ProbePool.Num() > 0)
        {
            SharedCaptureComp = ProbePool[0].CaptureComp;
            SharedDepthRT = ProbePool[0].DepthRT;
            SharedRGBRT = ProbePool[0].RGBRT;
        }
    }

    if (FisheyeParameters.AngleStep <= 0.01f) return;

    float H_Start = -FisheyeParameters.HorizontalFOV / 2.0f;
    float H_End = FisheyeParameters.HorizontalFOV / 2.0f;
    float V_Start = -FisheyeParameters.VerticalFOV / 2.0f;
    float V_End = FisheyeParameters.VerticalFOV / 2.0f;
    
    int32 ViewIndex = 0;
    
    for (float P = V_Start; P <= V_End + 0.01f; P += FisheyeParameters.AngleStep)
    {
        for (float Y = H_Start; Y <= H_End + 0.01f; Y += FisheyeParameters.AngleStep)
        {
            FMultiViewData Meta;
            Meta.ViewIndex = ViewIndex;
            Meta.Rotation = FRotator(P, Y, 0.0f);
            ViewsMetadata.Add(Meta);
            ViewIndex++;
        }
    }
    
    UE_LOG(LogTemp, Log, TEXT("MultiViewFisheye: Prepared metadata for %d views (Mode=%s, Probes=%d)."), 
        ViewsMetadata.Num(), 
        FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial ? TEXT("Serial") : TEXT("Parallel"),
        FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial ? 1 : ProbePool.Num());
}

void UMultiViewFisheyeCameraComponent::CaptureFisheyeScene()
{
    // 强制更新世界变换
    UpdateComponentToWorld();
    
    // 现在不再在一帧内并发捕获所有视图
    // 真正的渲染动作迁移到了 SaveAllData 中，以实现 渲染 -> 保存 -> 冲刷 -> 复用 RT 的串行流程
    // 这样做可以避免 20+ 个视图堆积在 GPU 导致的命令队列过载和显存溢出
    
    if (SharedCaptureComp)
    {
        // 渲染一次 Rig 中心视图用于预热世界空间状态 (Lumen Surface Cache)
        SharedCaptureComp->SetRelativeRotation(FRotator::ZeroRotator);
        SharedCaptureComp->UpdateComponentToWorld();
        SharedCaptureComp->CaptureScene();
        FlushRenderingCommands();
    }
}



bool UMultiViewFisheyeCameraComponent::SaveAllData(const FString& BasePath, int32 FrameNumber)
{
    if (!SharedCaptureComp || !SharedCaptureComp->TextureTarget) return false;

    int32 FrameNum = (FrameNumber >= 0) ? FrameNumber : FrameCounter;
    FString FrameFolder = FString::Printf(TEXT("%s/Frame_%04d"), *BasePath, FrameNum);
    IFileManager::Get().MakeDirectory(*FrameFolder, true);

    TArray<TSharedPtr<FJsonValue>> ViewsJsonArray;
    ViewsJsonArray.AddZeroed(ViewsMetadata.Num());

    static IConsoleVariable* VSMCacheCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Cache"));
    static IConsoleVariable* VSMInvalidateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.ForceInvalidate"));
    int32 OldVSMCache = VSMCacheCVar ? VSMCacheCVar->GetInt() : 1;

    // 确定使用的探针列表
    TArray<FMultiViewProbe> ActiveProbes;
    if (FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial)
    {
        FMultiViewProbe SerialProbe;
        SerialProbe.CaptureComp = SharedCaptureComp;
        SerialProbe.DepthRT = SharedDepthRT;
        ActiveProbes.Add(SerialProbe);
    }
    else
    {
        ActiveProbes = ProbePool;
    }

    int32 NumProbes = ActiveProbes.Num();

    // --- 【新增】：全局预热阶段 (Global Warmup) ---
    // 所有探针都在同一个世界位置，Lumen Surface Cache 和 VSM 只需要预热一次。
    if (NumProbes > 0 && FisheyeParameters.WarmUpFrames > 0)
    {
        FMultiViewProbe& WarmupProbe = ActiveProbes[0];
        
        float OldLightingQuality = WarmupProbe.CaptureComp->PostProcessSettings.LumenSceneLightingQuality;
        WarmupProbe.CaptureComp->PostProcessSettings.bOverride_LumenSceneLightingQuality = true;
        WarmupProbe.CaptureComp->PostProcessSettings.LumenSceneLightingQuality = 4.0f; // 提高单帧空间收敛质量

        for (int32 w = 0; w < FisheyeParameters.WarmUpFrames; w++)
        {
            if (FisheyeParameters.bEnableShadows)
            {
                // 仅首帧清空阴影缓存，后续帧持续累积
                if (VSMCacheCVar) VSMCacheCVar->Set(w == 0 ? 0 : 1, ECVF_SetByCode);
                if (VSMInvalidateCVar) VSMInvalidateCVar->Set(w == 0 ? 1 : 0, ECVF_SetByCode);
            }
            WarmupProbe.CaptureComp->CaptureScene();
            FlushRenderingCommands();
        }
        
        // 预热完成，确保缓存开启
        if (FisheyeParameters.bEnableShadows)
        {
            if (VSMCacheCVar) VSMCacheCVar->Set(1, ECVF_SetByCode);
            if (VSMInvalidateCVar) VSMInvalidateCVar->Set(0, ECVF_SetByCode);
        }
        
        for (int32 j = 0; j < NumProbes; j++)
        {
            ActiveProbes[j].CaptureComp->PostProcessSettings.bOverride_LumenSceneLightingQuality = true;
            ActiveProbes[j].CaptureComp->PostProcessSettings.LumenSceneLightingQuality = 4.0f;
        }
    }

    // 分组并行执行
    for (int32 i = 0; i < ViewsMetadata.Num(); i += NumProbes)
    {
        int32 CurrentBatchSize = FMath::Min(NumProbes, ViewsMetadata.Num() - i);

        // --- 第一步：先将所有探针对准目标方向 ---
        for (int32 j = 0; j < CurrentBatchSize; j++)
        {
            int32 ViewIdx = i + j;
            FMultiViewProbe& Probe = ActiveProbes[j];
            const FMultiViewData& Meta = ViewsMetadata[ViewIdx];

            Probe.CaptureComp->SetRelativeRotation(Meta.Rotation);
            Probe.CaptureComp->UpdateComponentToWorld();
        }

        // --- 第二步：最终采样帧 ---
        for (int32 j = 0; j < CurrentBatchSize; j++)
        {
            FMultiViewProbe& Probe = ActiveProbes[j];
            Probe.CaptureComp->CaptureScene();
        }
        FlushRenderingCommands();
        // --- 第二步：提取 RGB 并异步保存 ---
        for (int32 j = 0; j < CurrentBatchSize; j++)
        {
            int32 ViewIdx = i + j;
            FMultiViewProbe& Probe = ActiveProbes[j];
            const FMultiViewData& Meta = ViewsMetadata[ViewIdx];
            FString ImageBaseName = FString::Printf(TEXT("View_%03d_P%d_Y%d"), Meta.ViewIndex, (int)Meta.Rotation.Pitch, (int)Meta.Rotation.Yaw);

            FString FullPathRGB = FrameFolder / (ImageBaseName + TEXT(".png"));
            FImageWriteOptions RGBOptions;
            RGBOptions.Format = EDesiredImageFormat::PNG;
            RGBOptions.bAsync = false; 
            UImageWriteBlueprintLibrary::ExportToDisk(Probe.CaptureComp->TextureTarget, FullPathRGB, RGBOptions);

            // 记录元数据 (由 Probe 0 驱动或当前探针驱动)
            TSharedPtr<FJsonObject> ViewObj = MakeShareable(new FJsonObject);
            ViewObj->SetNumberField(TEXT("view_index"), Meta.ViewIndex);
            ViewObj->SetNumberField(TEXT("pitch"), Meta.Rotation.Pitch);
            ViewObj->SetNumberField(TEXT("yaw"), Meta.Rotation.Yaw);
            ViewObj->SetStringField(TEXT("filename"), ImageBaseName + TEXT(".png"));
            ViewObj->SetNumberField(TEXT("width"), Probe.CaptureComp->TextureTarget->SizeX);
            ViewObj->SetNumberField(TEXT("height"), Probe.CaptureComp->TextureTarget->SizeY);
            ViewObj->SetNumberField(TEXT("fov"), Probe.CaptureComp->FOVAngle);

            FVector Loc = Probe.CaptureComp->GetComponentLocation();
            FQuat Rot = Probe.CaptureComp->GetComponentQuat();
            TArray<TSharedPtr<FJsonValue>> PosArr;
            PosArr.Add(MakeShareable(new FJsonValueNumber(Loc.X)));
            PosArr.Add(MakeShareable(new FJsonValueNumber(Loc.Y)));
            PosArr.Add(MakeShareable(new FJsonValueNumber(Loc.Z)));
            ViewObj->SetArrayField(TEXT("world_pos"), PosArr);
            
            TArray<TSharedPtr<FJsonValue>> RotArr;
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.X)));
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.Y)));
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.Z)));
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.W)));
            ViewObj->SetArrayField(TEXT("world_rot_quat"), RotArr);

            // --- 第三步：渲染并导出 Depth (如果需要) ---
            if (FisheyeDepthPostProcessMaterial && Probe.DepthRT)
            {
                UTextureRenderTarget2D* OriginalRT = Probe.CaptureComp->TextureTarget;
                FPostProcessSettings OriginalPPSettings = Probe.CaptureComp->PostProcessSettings;
                ESceneCaptureSource OriginalSource = Probe.CaptureComp->CaptureSource;
                FEngineShowFlags OriginalShowFlags = Probe.CaptureComp->ShowFlags;

                Probe.CaptureComp->TextureTarget = Probe.DepthRT;
                Probe.CaptureComp->PostProcessSettings.WeightedBlendables.Array.Empty();
                // Probe.CaptureComp->PostProcessSettings.AddBlendable(FisheyeDepthPostProcessMaterial, 1.0f);
                PostProcessSettings.AddBlendable(FisheyeDepthPostProcessMaterial, 1.0f);
                Probe.CaptureComp->ShowFlags.SetTemporalAA(false);
                Probe.CaptureComp->ShowFlags.SetAntiAliasing(false);
                Probe.CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;

                Probe.CaptureComp->CaptureScene();
                FlushRenderingCommands();

                FString DepthFileName = ImageBaseName + TEXT("_Depth.exr");
                FImageWriteOptions DepthOptions;
                DepthOptions.Format = EDesiredImageFormat::EXR;
                DepthOptions.bAsync = false;
                UImageWriteBlueprintLibrary::ExportToDisk(Probe.DepthRT, FrameFolder / DepthFileName, DepthOptions);
                ViewObj->SetStringField(TEXT("depth_filename"), DepthFileName);

                Probe.CaptureComp->TextureTarget = OriginalRT;
                Probe.CaptureComp->PostProcessSettings = OriginalPPSettings;
                Probe.CaptureComp->CaptureSource = OriginalSource;
                Probe.CaptureComp->ShowFlags = OriginalShowFlags;
            }

            // --- 第四步：渲染并导出 Position (如果需要) ---
            if (FisheyePositionPostProcessMaterial && Probe.DepthRT)
            {
                UTextureRenderTarget2D* OriginalRT = Probe.CaptureComp->TextureTarget;
                FPostProcessSettings OriginalPPSettings = Probe.CaptureComp->PostProcessSettings;
                ESceneCaptureSource OriginalSource = Probe.CaptureComp->CaptureSource;
                FEngineShowFlags OriginalShowFlags = Probe.CaptureComp->ShowFlags;

                Probe.CaptureComp->TextureTarget = Probe.DepthRT;
                Probe.CaptureComp->PostProcessSettings.WeightedBlendables.Array.Empty();
                Probe.CaptureComp->PostProcessSettings.AddBlendable(FisheyePositionPostProcessMaterial, 1.0f);
                Probe.CaptureComp->ShowFlags.SetTemporalAA(false);
                Probe.CaptureComp->ShowFlags.SetAntiAliasing(false);
                Probe.CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;

                Probe.CaptureComp->CaptureScene();
                FlushRenderingCommands();

                FString PosFileName = ImageBaseName + TEXT("_Position.exr");
                FImageWriteOptions PosOptions;
                PosOptions.Format = EDesiredImageFormat::EXR;
                PosOptions.bAsync = false;
                UImageWriteBlueprintLibrary::ExportToDisk(Probe.DepthRT, FrameFolder / PosFileName, PosOptions);
                ViewObj->SetStringField(TEXT("position_filename"), PosFileName);

                Probe.CaptureComp->TextureTarget = OriginalRT;
                Probe.CaptureComp->PostProcessSettings = OriginalPPSettings;
                Probe.CaptureComp->CaptureSource = OriginalSource;
                Probe.CaptureComp->ShowFlags = OriginalShowFlags;
            }

            ViewsJsonArray[ViewIdx] = MakeShareable(new FJsonValueObject(ViewObj));
        }

        if (FisheyeParameters.bEnableShadows && VSMCacheCVar) VSMCacheCVar->Set(OldVSMCache, ECVF_SetByCode);
    }

    // 3. 保存 Summary Json
    FString JsonPath = FrameFolder / TEXT("views_metadata.json");
    TSharedPtr<FJsonObject> RootObj = MakeShareable(new FJsonObject());
    
    FVector ParentLoc = GetComponentLocation();
    FRotator ParentRot = GetComponentRotation();
    
    TSharedPtr<FJsonObject> LocObj = MakeShareable(new FJsonObject());
    LocObj->SetNumberField(TEXT("x"), ParentLoc.X);
    LocObj->SetNumberField(TEXT("y"), ParentLoc.Y);
    LocObj->SetNumberField(TEXT("z"), ParentLoc.Z);
    RootObj->SetObjectField(TEXT("parent_location"), LocObj);
    
    TSharedPtr<FJsonObject> RotObj = MakeShareable(new FJsonObject());
    RotObj->SetNumberField(TEXT("pitch"), ParentRot.Pitch);
    RotObj->SetNumberField(TEXT("yaw"), ParentRot.Yaw);
    RotObj->SetNumberField(TEXT("roll"), ParentRot.Roll);
    RootObj->SetObjectField(TEXT("parent_rotation"), RotObj);
    
    RootObj->SetArrayField(TEXT("views"), ViewsJsonArray);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    if (FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer))
    {
        FFileHelper::SaveStringToFile(JsonString, *JsonPath);
    }

    return true;
}
