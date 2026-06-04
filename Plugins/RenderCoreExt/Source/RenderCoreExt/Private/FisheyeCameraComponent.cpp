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

#include "FisheyeCameraComponent.h"
#include "Engine/SceneCapture.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ImageUtils.h"
#include "ImageWriteBlueprintLibrary.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/UnrealMemory.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"

UFisheyeCameraComponent::UFisheyeCameraComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // 禁用 Tick，由外部控制捕获
    CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    bCaptureEveryFrame = false; // 禁用自动捕获，由 Python 控制
    bAlwaysPersistRenderingState = true;

    // 默认渲染目标大小
    TextureTarget = CreateDefaultSubobject<UTextureRenderTarget2D>(TEXT("FisheyeRenderTarget"));
    TextureTarget->InitCustomFormat(1280, 720, PF_FloatRGBA, false); // 示例分辨率和格式

    // 创建深度渲染目标
    DepthRenderTarget = CreateDefaultSubobject<UTextureRenderTarget2D>(TEXT("FisheyeDepthRenderTarget"));
    DepthRenderTarget->InitCustomFormat(1280, 720, PF_FloatRGBA, false); // 使用32位浮点格式存储高精度深度

    // 创建深度Cubemap渲染目标
    DepthCubemapRenderTarget = CreateDefaultSubobject<UTextureRenderTargetCube>(TEXT("DepthCubemapRenderTarget"));
    DepthCubemapRenderTarget->Init(512, PF_FloatRGBA); // 每个面512x512，32位浮点

    // 创建RGB Cubemap渲染目标
    RGBCubemapRenderTarget = CreateDefaultSubobject<UTextureRenderTargetCube>(TEXT("RGBCubemapRenderTarget"));
    RGBCubemapRenderTarget->Init(512, PF_B8G8R8A8); // 每个面512x512，8位RGBA即可

    // 使用正常FOV，让后处理材质负责鱼眼畸变
    FOVAngle = 120.0; // 正常透视FOV，后处理材质会将其转换为鱼眼

    // 初始化帧计数器
    FrameCounter = 0;

    // 默认曝光和色调映射
    ExposureBias = 0.0f;
    bEnableToneCurve = true;
    bUseHDR = true;
    WarmUpFrames = 10;
    
    // 强制 SceneCapture 保持渲染状态，这是 Lumen 累积所必须的
    bAlwaysPersistRenderingState = true;
    bCaptureEveryFrame = false; // 我们手动 Tick，所以这里为 false，但我们在 pre-tick 做了处理


    // 启用 Tick
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    
    RemainingWarmUpFrames = 0;
    bIsCapturingSequence = false;

    // 初始化主组件的显示标志
    ShowFlags.Lighting = 1;
    ShowFlags.SkyLighting = 1;
    // ShowFlags.DirectionalLights = 1;
    ShowFlags.SetDirectionalLights(true);
    ShowFlags.PointLights = 1;
    ShowFlags.SpotLights = 1;
    ShowFlags.RectLights = 1;
    ShowFlags.Atmosphere = 1;
    ShowFlags.VolumetricFog = 1;
    ShowFlags.PostProcessing = 1;
    ShowFlags.GlobalIllumination = 1;
    ShowFlags.IndirectLightingCache = 1;
    ShowFlags.AmbientOcclusion = 1;
    ShowFlags.DynamicShadows = 1;
    ShowFlags.Landscape = 1;
    // ShowFlags.VirtualTexturePrimitives = 1;
    ShowFlags.SetVirtualTexturePrimitives(true);
    ShowFlags.Fog = 1;
    ShowFlags.LumenGlobalIllumination = 1;
    ShowFlags.LumenReflections = 1;
    ShowFlags.ScreenSpaceReflections = 1;
    // ShowFlags.Diffuse = 1;
    ShowFlags.SetDiffuse(true);
    ShowFlags.Specular = 1;
    // ShowFlags.DirectLighting = 1;
    ShowFlags.SetDirectLighting(true);
    ShowFlags.Bloom = 1;
    ShowFlags.ToneCurve = 1;
    // ShowFlags.ColorGrading = 1;
    ShowFlags.SetColorGrading(true);
    ShowFlags.Particles = 1;
    ShowFlags.SkeletalMeshes = 1;
    ShowFlags.StaticMeshes = 1;
    ShowFlags.InstancedStaticMeshes = 1;
    ShowFlags.InstancedFoliage = 1;
}

void UFisheyeCameraComponent::BeginPlay()
{
    Super::BeginPlay();

    UE_LOG(LogTemp, Warning, TEXT("=== FisheyeCamera BeginPlay ==="));
    UE_LOG(LogTemp, Warning, TEXT("FOV: %f"), FOVAngle);
    UE_LOG(LogTemp, Warning, TEXT("TextureTarget Valid: %s"), TextureTarget ? TEXT("Yes") : TEXT("No"));
    UE_LOG(LogTemp, Warning, TEXT("DepthRenderTarget Valid: %s"), DepthRenderTarget ? TEXT("Yes") : TEXT("No"));
    UE_LOG(LogTemp, Warning, TEXT("FisheyePostProcessMaterial Valid: %s"),
        FisheyePostProcessMaterial ? TEXT("Yes") : TEXT("No"));
        
    // 强制开启 Lumen 对 SceneCapture 的支持
    if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
    {
        PC->ConsoleCommand(TEXT("r.Lumen.SceneCapture.Allow 1")); // 关键：允许 SceneCapture 使用 Lumen
        PC->ConsoleCommand(TEXT("r.DynamicGlobalIlluminationMethod 1")); // 强制 Lumen
        PC->ConsoleCommand(TEXT("r.ReflectionMethod 1")); // 强制 Lumen 反射
        PC->ConsoleCommand(TEXT("r.Shadow.Virtual.Enable 0")); // 虚拟阴影贴图
        
        // 强制最高画质
        PC->ConsoleCommand(TEXT("sg.GlobalIlluminationQuality 2"));
        PC->ConsoleCommand(TEXT("sg.ReflectionQuality 2"));
    }
    UE_LOG(LogTemp, Warning, TEXT("FisheyeDepthPostProcessMaterial Valid: %s"),
        FisheyeDepthPostProcessMaterial ? TEXT("Yes") : TEXT("No"));

    // 加载并应用后处理材质
    if (FisheyePostProcessMaterial)
    {
        UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(FisheyePostProcessMaterial, this);
        if (DynamicMaterial)
        {
            // 添加到后处理设置
            // PostProcessSettings.AddBlendable(DynamicMaterial, 1.0f);
            PostProcessSettings.AddBlendable(DynamicMaterial, 1.0f);
            UE_LOG(LogTemp, Warning, TEXT("✓ Post-process material '%s' applied"),
                *FisheyePostProcessMaterial->GetName());
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("✗ Failed to create dynamic material"));
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("✗ FisheyePostProcessMaterial is NULL!"));
    }
    
    // 同步参数
    SyncParameters();
    
    // Initial capture
    CaptureScene();
    UE_LOG(LogTemp, Warning, TEXT("Initial capture completed"));
}

void UFisheyeCameraComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (bIsCapturingSequence && RemainingWarmUpFrames > 0)
    {
        // 每一帧都进行捕获，让 Lumen 和 TAA 积累
        if (FisheyeRGBFromCubemapMaterial)
        {
            CaptureRGBCubemap();
        }
        else
        {
            CaptureScene();
        }
        
        RemainingWarmUpFrames--;

        if (RemainingWarmUpFrames == 0)
        {
            // 最后一帧：如果是鱼眼则执行最终渲染
            if (FisheyeRGBFromCubemapMaterial)
            {
                RenderFisheyeRGBFromCubemap();
            }
            else
            {
                CaptureScene();
            }
            
            // 如果有待处理的保存请求，则执行保存
            if (!PendingExportBasePath.IsEmpty())
            {
                // 暂时禁用 bIsCapturingSequence 以允许 SaveAllData 正常执行
                bool bOriginalBusy = bIsCapturingSequence;
                bIsCapturingSequence = false;
                SaveAllData(PendingExportBasePath, PendingFrameNumber);
                PendingExportBasePath = TEXT("");
                PendingFrameNumber = -1;
            }
            else
            {
                bIsCapturingSequence = false;
            }
            
            UE_LOG(LogTemp, Log, TEXT("Camera capture sequence finished."));
        }
    }
}

#if WITH_EDITOR
void UFisheyeCameraComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    FName PropertyName = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;
    
    if (PropertyName == GET_MEMBER_NAME_CHECKED(UFisheyeCameraComponent, FisheyeParameters) ||
        PropertyName == GET_MEMBER_NAME_CHECKED(UFisheyeCameraComponent, CubemapResolution))
    {
        SyncParameters();
    }
}
#endif

void UFisheyeCameraComponent::SyncParameters()
{
    // 1. 更新分辨率 (支持超级采样)
    int32 RenderWidth = FMath::RoundToInt(FisheyeParameters.Resolution.X * SupersamplingMultiplier);
    int32 RenderHeight = FMath::RoundToInt(FisheyeParameters.Resolution.Y * SupersamplingMultiplier);

    if (TextureTarget && (TextureTarget->SizeX != RenderWidth || TextureTarget->SizeY != RenderHeight))
    {
        TextureTarget->ResizeTarget(RenderWidth, RenderHeight);
    }
    if (DepthRenderTarget && (DepthRenderTarget->SizeX != RenderWidth || DepthRenderTarget->SizeY != RenderHeight))
    {
        DepthRenderTarget->ResizeTarget(RenderWidth, RenderHeight);
    }


    // 1.6 增强 Lumen GI 设置
    PostProcessSettings.bOverride_LumenSceneLightingQuality = true;
    PostProcessSettings.LumenSceneLightingQuality = 2.0f;
    PostProcessSettings.bOverride_LumenSceneDetail = true;
    PostProcessSettings.LumenSceneDetail = 2.0f;
    PostProcessSettings.bOverride_LumenFinalGatherQuality = true;
    PostProcessSettings.LumenFinalGatherQuality = 2.0f;

    // 2. 更新后处理材质参数
    auto UpdateMaterialParams = [&](UMaterialInterface* MaterialInterface)
    {
        if (!MaterialInterface) return;
        
        for (auto& WeightedBlendable : PostProcessSettings.WeightedBlendables.Array)
        {
            UMaterialInstanceDynamic* DynamicMat = Cast<UMaterialInstanceDynamic>(WeightedBlendable.Object);
            if (DynamicMat && (DynamicMat->Parent == MaterialInterface || Cast<UMaterialInterface>(DynamicMat->GetBaseMaterial()) == MaterialInterface))
            {
                DynamicMat->SetScalarParameterValue("Fx", FisheyeParameters.Fx);
                DynamicMat->SetScalarParameterValue("Fy", FisheyeParameters.Fy);
                DynamicMat->SetScalarParameterValue("Cx", FisheyeParameters.Cx);
                DynamicMat->SetScalarParameterValue("Cy", FisheyeParameters.Cy);
                DynamicMat->SetScalarParameterValue("K1", FisheyeParameters.K1);
                DynamicMat->SetScalarParameterValue("K2", FisheyeParameters.K2);
                DynamicMat->SetScalarParameterValue("K3", FisheyeParameters.K3);
                DynamicMat->SetScalarParameterValue("K4", FisheyeParameters.K4);
                DynamicMat->SetScalarParameterValue("ImageWidth", (float)FisheyeParameters.Resolution.X);
                DynamicMat->SetScalarParameterValue("ImageHeight", (float)FisheyeParameters.Resolution.Y);
            }
        }
    };

    UpdateMaterialParams(FisheyePostProcessMaterial);
    
    // 3. 更新 Cubemap 分辨率和格式
    EPixelFormat RGBFormat = bUseHDR ? PF_FloatRGBA : PF_B8G8R8A8;
    if (RGBCubemapRenderTarget && (RGBCubemapRenderTarget->SizeX != CubemapResolution || RGBCubemapRenderTarget->GetFormat() != RGBFormat))
    {
        RGBCubemapRenderTarget->Init(CubemapResolution, RGBFormat);
    }
    if (DepthCubemapRenderTarget && DepthCubemapRenderTarget->SizeX != CubemapResolution)
    {
        DepthCubemapRenderTarget->Init(CubemapResolution, PF_FloatRGBA);
    }
}

void UFisheyeCameraComponent::ConfigureCamera(const FFisheyeParams& Params)
{
    FisheyeParameters = Params;
    ExposureBias = Params.ExposureBias;
    bEnableToneCurve = Params.bEnableToneCurve;
    bUseHDR = Params.bUseHDR;
    WarmUpFrames = Params.WarmUpFrames;
    SupersamplingMultiplier = Params.SupersamplingMultiplier;
    
    SyncParameters();
    UE_LOG(LogTemp, Log, TEXT("Camera configured: %s (%dx%d), HDR=%d, WarmUp=%d"), 
        *Params.CameraModel, Params.Resolution.X, Params.Resolution.Y, bUseHDR, WarmUpFrames);
}

FVector UFisheyeCameraComponent::PixelToRay(FVector2D PixelCoord) const
{
    float x = (PixelCoord.X - FisheyeParameters.Cx) / FisheyeParameters.Fx;
    float y = (PixelCoord.Y - FisheyeParameters.Cy) / FisheyeParameters.Fy;
    float r = FMath::Sqrt(x * x + y * y);

    if (r < KINDA_SMALL_NUMBER) return FVector(1.0f, 0.0f, 0.0f);

    float theta = r; 
    float phi = FMath::Atan2(y, x);

    float SinTheta = FMath::Sin(theta);
    float CosTheta = FMath::Cos(theta);

    return FVector(
        CosTheta,                   // X (Forward)
        SinTheta * FMath::Cos(phi), // Y (Right)
        SinTheta * FMath::Sin(phi)  // Z (Up)
    );
}

void UFisheyeCameraComponent::CaptureFisheyeScene()
{
    UWorld* World = GetWorld();
    if (!World) return;

    // 关键：Lumen 需要历史帧累积
    bAlwaysPersistRenderingState = true;

    SyncParameters();

    if (!TextureTarget)
    {
        TextureTarget = NewObject<UTextureRenderTarget2D>(this);
        TextureTarget->InitCustomFormat(FisheyeParameters.Resolution.X, FisheyeParameters.Resolution.Y, PF_B8G8R8A8, false);
    }
    if (!DepthRenderTarget)
    {
        DepthRenderTarget = NewObject<UTextureRenderTarget2D>(this);
        DepthRenderTarget->InitCustomFormat(FisheyeParameters.Resolution.X, FisheyeParameters.Resolution.Y, PF_FloatRGBA, false);
    }
    if (!RGBCubemapRenderTarget)
    {
        RGBCubemapRenderTarget = NewObject<UTextureRenderTargetCube>(this);
        RGBCubemapRenderTarget->Init(CubemapResolution, PF_B8G8R8A8);
    }
    if (!DepthCubemapRenderTarget)
    {
        DepthCubemapRenderTarget = NewObject<UTextureRenderTargetCube>(this);
        DepthCubemapRenderTarget->Init(CubemapResolution, PF_FloatRGBA);
    }

    UE_LOG(LogTemp, Log, TEXT("CaptureFisheyeScene: RGBMat=%s, DepthMat=%s, UseCubemap=%d"), 
        FisheyeRGBFromCubemapMaterial ? *FisheyeRGBFromCubemapMaterial->GetName() : TEXT("None"),
        FisheyeDepthFromCubemapMaterial ? *FisheyeDepthFromCubemapMaterial->GetName() : TEXT("None"),
        bUseCubemapForDepth);

    World->SendAllEndOfFrameUpdates();
    FlushRenderingCommands();
    UpdateComponentToWorld();

    // 强制确保 GI 和 Lumen 开启
    ShowFlags.SetLighting(true);
    ShowFlags.SetGlobalIllumination(true);
    ShowFlags.SetLumenGlobalIllumination(true);
    ShowFlags.SetLumenReflections(true);

    // 启动预热序列：由 TickComponent 处理后续帧
    if (WarmUpFrames > 0)
    {
        RemainingWarmUpFrames = WarmUpFrames;
        bIsCapturingSequence = true;
        UE_LOG(LogTemp, Log, TEXT("Starting camera capture sequence with %d warm-up frames"), WarmUpFrames);
    }
    else
    {
        if (RGBCubemapRenderTarget && FisheyeRGBFromCubemapMaterial)
        {
            CaptureRGBCubemap();
            RenderFisheyeRGBFromCubemap();
        }
        else
        {
            CaptureScene();
        }
    }

    if (bUseCubemapForDepth && DepthCubemapRenderTarget && FisheyeDepthFromCubemapMaterial)
    {
        CaptureDepthCubemap();
        RenderFisheyeDepthFromCubemap();
    }
    else if (DepthRenderTarget && FisheyeDepthPostProcessMaterial)
    {
        ESceneCaptureSource OriginalSource = CaptureSource;
        UTextureRenderTarget2D* OriginalTarget = TextureTarget;
        FPostProcessSettings OriginalSettings = PostProcessSettings;

        CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
        TextureTarget = DepthRenderTarget;
        PostProcessSettings.WeightedBlendables.Array.Empty();
        
        UMaterialInstanceDynamic* DepthDynamicMaterial = UMaterialInstanceDynamic::Create(FisheyeDepthPostProcessMaterial, this);
        if (DepthDynamicMaterial)
        {
            DepthDynamicMaterial->SetScalarParameterValue("Fx", FisheyeParameters.Fx);
            DepthDynamicMaterial->SetScalarParameterValue("Fy", FisheyeParameters.Fy);
            DepthDynamicMaterial->SetScalarParameterValue("Cx", FisheyeParameters.Cx);
            DepthDynamicMaterial->SetScalarParameterValue("Cy", FisheyeParameters.Cy);
            DepthDynamicMaterial->SetScalarParameterValue("K1", FisheyeParameters.K1);
            DepthDynamicMaterial->SetScalarParameterValue("K2", FisheyeParameters.K2);
            DepthDynamicMaterial->SetScalarParameterValue("K3", FisheyeParameters.K3);
            DepthDynamicMaterial->SetScalarParameterValue("K4", FisheyeParameters.K4);
            DepthDynamicMaterial->SetScalarParameterValue("ImageWidth", (float)DepthRenderTarget->SizeX);
            DepthDynamicMaterial->SetScalarParameterValue("ImageHeight", (float)DepthRenderTarget->SizeY);
            // PostProcessSettings.AddBlendable(DepthDynamicMaterial, 1.0f);
            PostProcessSettings.AddBlendable(DepthDynamicMaterial, 1.0f);
        }

        PostProcessSettings.bOverride_AutoExposureMethod = true;
        PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        PostProcessSettings.bOverride_AutoExposureBias = true;
        PostProcessSettings.AutoExposureBias = 0.0f;
        PostProcessSettings.bOverride_ToneCurveAmount = true;
        PostProcessSettings.ToneCurveAmount = 0.0f;

        CaptureScene();

        CaptureSource = OriginalSource;
        TextureTarget = OriginalTarget;
        PostProcessSettings = OriginalSettings;
    }
    else if (DepthRenderTarget)
    {
        ESceneCaptureSource OriginalSource = CaptureSource;
        UTextureRenderTarget2D* OriginalTarget = TextureTarget;
        CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
        TextureTarget = DepthRenderTarget;
        CaptureScene();
        CaptureSource = OriginalSource;
        TextureTarget = OriginalTarget;
    }

    FrameCounter++;

    if (bAutoSaveOnCapture)
    {
        FString FramePath = FString::Printf(TEXT("%s/frame_%05d"), *ExportBasePath, FrameCounter);
        SaveAllData(FramePath, FrameCounter);
    }

    UE_LOG(LogTemp, Log, TEXT("Fisheye scene captured (Frame: %d)"), FrameCounter);
}

void UFisheyeCameraComponent::CaptureDepthCubemap()
{
    if (!DepthCubeCaptureComponent)
    {
        DepthCubeCaptureComponent = NewObject<USceneCaptureComponentCube>(this);
        if (DepthCubeCaptureComponent)
        {
            DepthCubeCaptureComponent->SetupAttachment(this);
            DepthCubeCaptureComponent->RegisterComponent();
            DepthCubeCaptureComponent->bCaptureEveryFrame = false;
            DepthCubeCaptureComponent->bCaptureOnMovement = false;
            DepthCubeCaptureComponent->SetActive(true);

            FEngineShowFlags& Flags = DepthCubeCaptureComponent->ShowFlags;
            Flags.StaticMeshes = 1;
            Flags.Landscape = 1;
            // Flags.VirtualTexturePrimitives = 1;
            Flags.SetVirtualTexturePrimitives(true);
            Flags.InstancedStaticMeshes = 1;
            Flags.InstancedFoliage = 1;
        }
    }
    
    if (!DepthCubeCaptureComponent || !DepthCubemapRenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("CaptureDepthCubemap: Missing resources"));
        return;
    }
    
    if (DepthCubemapRenderTarget->SizeX != CubemapResolution)
    {
        DepthCubemapRenderTarget->Init(CubemapResolution, PF_FloatRGBA);
    }
    
    DepthCubeCaptureComponent->SetRelativeLocation(FVector::ZeroVector);
    DepthCubeCaptureComponent->SetWorldRotation(FRotator::ZeroRotator);
    DepthCubeCaptureComponent->UpdateComponentToWorld();
    
    FlushRenderingCommands();
    
    DepthCubeCaptureComponent->TextureTarget = DepthCubemapRenderTarget;
    DepthCubeCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
    DepthCubeCaptureComponent->bCaptureEveryFrame = false;
    DepthCubeCaptureComponent->MarkRenderStateDirty();
    DepthCubeCaptureComponent->CaptureScene();
    
    FlushRenderingCommands();
}

void UFisheyeCameraComponent::RenderFisheyeDepthFromCubemap()
{
    if (!DepthRenderTarget || !FisheyeDepthFromCubemapMaterial || !DepthCubemapRenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("RenderFisheyeDepthFromCubemap: Missing resources"));
        return;
    }
    
    UWorld* World = GetWorld();
    if (!World) return;
    
    if (!DepthCubemapMID || DepthCubemapMID->Parent != FisheyeDepthFromCubemapMaterial)
    {
        DepthCubemapMID = UMaterialInstanceDynamic::Create(FisheyeDepthFromCubemapMaterial, this);
    }
    
    if (!DepthCubemapMID) return;
    
    UKismetRenderingLibrary::ClearRenderTarget2D(World, DepthRenderTarget);
    
    DepthCubemapMID->SetTextureParameterValue("DepthCubemap", DepthCubemapRenderTarget);
    DepthCubemapMID->SetScalarParameterValue("Fx", FisheyeParameters.Fx);
    DepthCubemapMID->SetScalarParameterValue("Fy", FisheyeParameters.Fy);
    DepthCubemapMID->SetScalarParameterValue("Cx", FisheyeParameters.Cx);
    DepthCubemapMID->SetScalarParameterValue("Cy", FisheyeParameters.Cy);
    DepthCubemapMID->SetScalarParameterValue("K1", FisheyeParameters.K1);
    DepthCubemapMID->SetScalarParameterValue("K2", FisheyeParameters.K2);
    DepthCubemapMID->SetScalarParameterValue("K3", FisheyeParameters.K3);
    DepthCubemapMID->SetScalarParameterValue("K4", FisheyeParameters.K4);
    DepthCubemapMID->SetScalarParameterValue("ImageWidth", (float)DepthRenderTarget->SizeX);
    DepthCubemapMID->SetScalarParameterValue("ImageHeight", (float)DepthRenderTarget->SizeY);
    
    FVector Forward = GetForwardVector();
    FVector Right = GetRightVector();
    FVector Up = GetUpVector();
    
    DepthCubemapMID->SetVectorParameterValue("CameraForward", FLinearColor(Forward));
    DepthCubemapMID->SetVectorParameterValue("CameraRight", FLinearColor(Right));
    DepthCubemapMID->SetVectorParameterValue("CameraUp", FLinearColor(Up));
    
    UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, DepthRenderTarget, DepthCubemapMID);
    FlushRenderingCommands();
}

void UFisheyeCameraComponent::CaptureRGBCubemap()
{
    if (!RGBCubeCaptureComponent)
    {
        RGBCubeCaptureComponent = NewObject<USceneCaptureComponentCube>(this);
        if (RGBCubeCaptureComponent)
        {
            RGBCubeCaptureComponent->SetupAttachment(this);
            RGBCubeCaptureComponent->RegisterComponent();
            RGBCubeCaptureComponent->bCaptureEveryFrame = false;
            RGBCubeCaptureComponent->bCaptureOnMovement = false;
            RGBCubeCaptureComponent->SetActive(true);

            FEngineShowFlags& Flags = RGBCubeCaptureComponent->ShowFlags;
            Flags.Lighting = 1;
            Flags.SkyLighting = 1;
            // Flags.DirectionalLights = 1;
            Flags.SetDirectionalLights(true);
            Flags.PointLights = 1;
            Flags.SpotLights = 1;
            Flags.RectLights = 1;
            Flags.Atmosphere = 1;
            Flags.VolumetricFog = 1;
            Flags.PostProcessing = 1;
            Flags.GlobalIllumination = 1;
            Flags.IndirectLightingCache = 1;
            Flags.AmbientOcclusion = 1;
            Flags.DynamicShadows = 1;
            Flags.StaticMeshes = 1;
            Flags.Landscape = 1;
            // Flags.VirtualTexturePrimitives = 1;
            Flags.SetVirtualTexturePrimitives(true);
            Flags.Fog = 1;
            Flags.InstancedStaticMeshes = 1;
            Flags.InstancedFoliage = 1;
            Flags.LumenGlobalIllumination = 1;
            Flags.LumenReflections = 1;
            Flags.ScreenSpaceReflections = 1;
            // Flags.Diffuse = 1;
            Flags.SetDiffuse(true);
            Flags.Specular = 1;
            // Flags.DirectLighting = 1;
            Flags.SetDirectLighting(true);
            Flags.Bloom = 1;
            Flags.DepthOfField = 1;
            Flags.EyeAdaptation = 1;
            Flags.MotionBlur = 1;
            Flags.ToneCurve = 1;
            Flags.LocalExposure = 1;
            // Flags.SceneColorFringe = 1;
            Flags.SetSceneColorFringe(true);
            // Flags.CameraImperfections = 1;
            Flags.SetCameraImperfections(true);
            // Flags.LensFlares = 1;
            Flags.SetLensFlares(true);
            // Flags.ColorGrading = 1;
            Flags.SetColorGrading(true);
            Flags.TextRender = 1;
            Flags.Particles = 1;
            Flags.SkeletalMeshes = 1;
        }
    }
    
    if (!RGBCubeCaptureComponent || !RGBCubemapRenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("CaptureRGBCubemap: Missing resources"));
        return;
    }
    
    if (RGBCubemapRenderTarget->SizeX != CubemapResolution)
    {
        RGBCubemapRenderTarget->Init(CubemapResolution, PF_B8G8R8A8);
    }
    
    RGBCubeCaptureComponent->SetRelativeLocation(FVector::ZeroVector);
    RGBCubeCaptureComponent->SetWorldRotation(FRotator::ZeroRotator);
    RGBCubeCaptureComponent->UpdateComponentToWorld();
    
    FlushRenderingCommands();
    
    RGBCubeCaptureComponent->TextureTarget = RGBCubemapRenderTarget;
    RGBCubeCaptureComponent->CaptureSource = bUseHDR ? ESceneCaptureSource::SCS_SceneColorHDR : ESceneCaptureSource::SCS_FinalColorLDR;
    RGBCubeCaptureComponent->bCaptureEveryFrame = false;
    
    RGBCubeCaptureComponent->PostProcessSettings.bOverride_AutoExposureMethod = true;
    RGBCubeCaptureComponent->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
    RGBCubeCaptureComponent->PostProcessSettings.bOverride_AutoExposureBias = true;
    RGBCubeCaptureComponent->PostProcessSettings.AutoExposureBias = ExposureBias;
    
    // 2. 移除强制禁用 Bloom 和 Vignette 的限制，以匹配视口效果
    // 注意：这可能会在 Cubemap 面之间产生轻微的拼接缝隙
    RGBCubeCaptureComponent->PostProcessSettings.bOverride_ToneCurveAmount = true;
    RGBCubeCaptureComponent->PostProcessSettings.ToneCurveAmount = bEnableToneCurve ? 1.0f : 0.0f;
    
    RGBCubeCaptureComponent->bAlwaysPersistRenderingState = true;
    RGBCubeCaptureComponent->MarkRenderStateDirty();
    RGBCubeCaptureComponent->CaptureScene();
    
    FlushRenderingCommands();
}

void UFisheyeCameraComponent::RenderFisheyeRGBFromCubemap()
{
    if (!TextureTarget || !FisheyeRGBFromCubemapMaterial || !RGBCubemapRenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("RenderFisheyeRGBFromCubemap: Missing resources"));
        return;
    }
    
    UWorld* World = GetWorld();
    if (!World) return;
    
    if (!RGBCubemapMID || RGBCubemapMID->Parent != FisheyeRGBFromCubemapMaterial)
    {
        RGBCubemapMID = UMaterialInstanceDynamic::Create(FisheyeRGBFromCubemapMaterial, this);
    }
    
    if (!RGBCubemapMID) return;
    
    UKismetRenderingLibrary::ClearRenderTarget2D(World, TextureTarget);
    
    RGBCubemapMID->SetTextureParameterValue("RGBCubemap", RGBCubemapRenderTarget);
    RGBCubemapMID->SetScalarParameterValue("Fx", FisheyeParameters.Fx);
    RGBCubemapMID->SetScalarParameterValue("Fy", FisheyeParameters.Fy);
    RGBCubemapMID->SetScalarParameterValue("Cx", FisheyeParameters.Cx);
    RGBCubemapMID->SetScalarParameterValue("Cy", FisheyeParameters.Cy);
    RGBCubemapMID->SetScalarParameterValue("K1", FisheyeParameters.K1);
    RGBCubemapMID->SetScalarParameterValue("K2", FisheyeParameters.K2);
    RGBCubemapMID->SetScalarParameterValue("K3", FisheyeParameters.K3);
    RGBCubemapMID->SetScalarParameterValue("K4", FisheyeParameters.K4);
    RGBCubemapMID->SetScalarParameterValue("ImageWidth", (float)TextureTarget->SizeX);
    RGBCubemapMID->SetScalarParameterValue("ImageHeight", (float)TextureTarget->SizeY);
    
    // 新增：传递 HDR 和色调映射标志
    RGBCubemapMID->SetScalarParameterValue("bUseHDR", bUseHDR ? 1.0f : 0.0f);
    RGBCubemapMID->SetScalarParameterValue("bEnableToneCurve", bEnableToneCurve ? 1.0f : 0.0f);
    RGBCubemapMID->SetScalarParameterValue("ExposureBias", ExposureBias);
    
    FVector Forward = GetForwardVector();
    FVector Right = GetRightVector();
    FVector Up = GetUpVector();
    
    RGBCubemapMID->SetVectorParameterValue("CameraForward", FLinearColor(Forward));
    RGBCubemapMID->SetVectorParameterValue("CameraRight", FLinearColor(Right));
    RGBCubemapMID->SetVectorParameterValue("CameraUp", FLinearColor(Up));
    
    UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, TextureTarget, RGBCubemapMID);
    FlushRenderingCommands();
}

bool UFisheyeCameraComponent::ReadRenderTargetPixels(UTextureRenderTarget2D* RenderTarget, TArray<FColor>& OutPixels)
{
    if (!RenderTarget) return false;
    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource) return false;

    FIntPoint Size(RenderTarget->SizeX, RenderTarget->SizeY);
    OutPixels.SetNumUninitialized(Size.X * Size.Y);
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
    ReadFlags.SetLinearToGamma(false);
    RTResource->ReadPixels(OutPixels, ReadFlags);
    return true;
}

bool UFisheyeCameraComponent::ReadDepthRenderTargetPixels(UTextureRenderTarget2D* RenderTarget, TArray<float>& OutDepthValues)
{
    if (!RenderTarget) return false;
    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource) return false;

    FIntPoint Size(RenderTarget->SizeX, RenderTarget->SizeY);
    TArray<FColor> DepthPixels;
    DepthPixels.SetNumUninitialized(Size.X * Size.Y);
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
    ReadFlags.SetLinearToGamma(false);
    RTResource->ReadPixels(DepthPixels, ReadFlags);

    OutDepthValues.SetNumUninitialized(Size.X * Size.Y);
    for (int32 i = 0; i < DepthPixels.Num(); i++)
    {
        OutDepthValues[i] = (float)DepthPixels[i].R / 255.0f;
    }
    return true;
}

bool UFisheyeCameraComponent::ReadDepthRenderTargetPixelsHighPrecision(UTextureRenderTarget2D* RenderTarget, TArray<float>& OutDepthValues)
{
    if (!RenderTarget) return false;
    FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
    if (!RTResource) return false;

    FIntPoint Size(RenderTarget->SizeX, RenderTarget->SizeY);
    TArray<FLinearColor> FloatPixels;
    FloatPixels.SetNumUninitialized(Size.X * Size.Y);
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
    ReadFlags.SetLinearToGamma(false);

    if (!RTResource->ReadLinearColorPixels(FloatPixels, ReadFlags)) return false;

    OutDepthValues.SetNumUninitialized(Size.X * Size.Y);
    for (int32 i = 0; i < FloatPixels.Num(); i++)
    {
        OutDepthValues[i] = FloatPixels[i].R;
    }
    return true;
}

bool UFisheyeCameraComponent::SaveRGBImage(const FString& FilePath, int32 FrameNumber)
{
    if (!TextureTarget) return false;
    TArray<FColor> Pixels;
    if (!ReadRenderTargetPixels(TextureTarget, Pixels)) return false;

    FIntPoint Size(TextureTarget->SizeX, TextureTarget->SizeY);
    FString FullPath = (FrameNumber >= 0) ? FString::Printf(TEXT("%s_rgb_%05d.png"), *FilePath, FrameNumber) : FString::Printf(TEXT("%s_rgb.png"), *FilePath);

    FString Directory = FPaths::GetPath(FullPath);
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);

    return FFileHelper::CreateBitmap(*FullPath, Size.X, Size.Y, Pixels.GetData());
}

bool UFisheyeCameraComponent::SaveDepthImage(const FString& FilePath, int32 FrameNumber)
{
    if (!DepthRenderTarget) return false;
    TArray<float> DepthValues;
    if (!ReadDepthRenderTargetPixelsHighPrecision(DepthRenderTarget, DepthValues)) return false;

    FIntPoint Size(DepthRenderTarget->SizeX, DepthRenderTarget->SizeY);
    TArray<FColor> DepthPixels;
    DepthPixels.SetNumUninitialized(Size.X * Size.Y);

    float MinDepth = TNumericLimits<float>::Max();
    float MaxDepth = TNumericLimits<float>::Lowest();
    for (float Depth : DepthValues)
    {
        if (Depth > KINDA_SMALL_NUMBER && Depth < 10000000.0f)
        {
            MinDepth = FMath::Min(MinDepth, Depth);
            MaxDepth = FMath::Max(MaxDepth, Depth);
        }
    }
    if (MinDepth > MaxDepth) { MinDepth = 0.0f; MaxDepth = 1.0f; }
    float DepthRange = FMath::Max(MaxDepth - MinDepth, KINDA_SMALL_NUMBER);

    for (int32 i = 0; i < DepthValues.Num(); i++)
    {
        float Normalized = FMath::Clamp((DepthValues[i] - MinDepth) / DepthRange, 0.0f, 1.0f);
        uint8 Gray = (uint8)(Normalized * 255.0f);
        DepthPixels[i] = FColor(Gray, Gray, Gray, 255);
    }

    FString FullPath = (FrameNumber >= 0) ? FString::Printf(TEXT("%s_depth_%05d.png"), *FilePath, FrameNumber) : FString::Printf(TEXT("%s_depth.png"), *FilePath);
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(FullPath));
    return FFileHelper::CreateBitmap(*FullPath, Size.X, Size.Y, DepthPixels.GetData());
}

void UFisheyeCameraComponent::WritePoseToFile(const FString& FilePath, const FVector& Location, const FQuat& Rotation, int32 FrameNumber)
{
    TSharedRef<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
    JsonObject->SetNumberField(TEXT("frame"), FrameNumber);
    
    auto SetVec = [&](const FString& Name, const FVector& Vec) {
        TSharedRef<FJsonObject> Obj = MakeShareable(new FJsonObject);
        Obj->SetNumberField(TEXT("x"), Vec.X); Obj->SetNumberField(TEXT("y"), Vec.Y); Obj->SetNumberField(TEXT("z"), Vec.Z);
        JsonObject->SetObjectField(Name, Obj);
    };
    SetVec(TEXT("position"), Location);
    
    TSharedRef<FJsonObject> RotObj = MakeShareable(new FJsonObject);
    RotObj->SetNumberField(TEXT("x"), Rotation.X); RotObj->SetNumberField(TEXT("y"), Rotation.Y); RotObj->SetNumberField(TEXT("z"), Rotation.Z); RotObj->SetNumberField(TEXT("w"), Rotation.W);
    JsonObject->SetObjectField(TEXT("rotation"), RotObj);

    TSharedRef<FJsonObject> ParamsObj = MakeShareable(new FJsonObject);
    ParamsObj->SetStringField(TEXT("camera_model"), FisheyeParameters.CameraModel);
    TArray<TSharedPtr<FJsonValue>> ResArr; ResArr.Add(MakeShareable(new FJsonValueNumber(FisheyeParameters.Resolution.X))); ResArr.Add(MakeShareable(new FJsonValueNumber(FisheyeParameters.Resolution.Y)));
    ParamsObj->SetArrayField(TEXT("resolution"), ResArr);
    
    JsonObject->SetObjectField(TEXT("camera_parameters"), ParamsObj);

    FString OutputString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    FJsonSerializer::Serialize(JsonObject, Writer);
    FString FullPath = (FrameNumber >= 0) ? FString::Printf(TEXT("%s_pose_%05d.json"), *FilePath, FrameNumber) : FString::Printf(TEXT("%s_pose.json"), *FilePath);
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(FullPath));
    FFileHelper::SaveStringToFile(OutputString, *FullPath);
}

bool UFisheyeCameraComponent::SaveDepthImageRaw(const FString& FilePath, int32 FrameNumber)
{
    if (!DepthRenderTarget) return false;
    TArray<float> DepthValues;
    if (!ReadDepthRenderTargetPixelsHighPrecision(DepthRenderTarget, DepthValues)) return false;

    FString FullPath = (FrameNumber >= 0) ? FString::Printf(TEXT("%s_depth_%05d.raw"), *FilePath, FrameNumber) : FString::Printf(TEXT("%s_depth.raw"), *FilePath);
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(FullPath));

    TArray<uint8> ByteArray;
    ByteArray.SetNumUninitialized(DepthValues.Num() * sizeof(float));
    FMemory::Memcpy(ByteArray.GetData(), DepthValues.GetData(), ByteArray.Num());
    return FFileHelper::SaveArrayToFile(ByteArray, *FullPath);
}

bool UFisheyeCameraComponent::SaveDepthImageHighPrecision(const FString& FilePath, int32 FrameNumber, bool bSaveAsEXR)
{
    if (!DepthRenderTarget) return false;
    TArray<float> DepthValues;
    if (!ReadDepthRenderTargetPixelsHighPrecision(DepthRenderTarget, DepthValues)) return false;

    FIntPoint Size(DepthRenderTarget->SizeX, DepthRenderTarget->SizeY);
    
    // 目前简化实现：如果是EXR，由于UE5原生没有简单的EXR保存函数，我们暂时也保存为PNG
    // 或者如果用户需要真正的16位，通常建议使用 SaveDepthImageRaw
    // 这里我们保存为一个16位精度的灰度图（如果支持）或者高精度的PNG
    
    FString Extension = bSaveAsEXR ? TEXT("exr") : TEXT("png");
    FString FullPath = (FrameNumber >= 0) ? FString::Printf(TEXT("%s_depth_hp_%05d.%s"), *FilePath, FrameNumber, *Extension) : FString::Printf(TEXT("%s_depth_hp.%s"), *FilePath, *Extension);
    
    FString Directory = FPaths::GetPath(FullPath);
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);

    // 转换为8位灰度保存（作为占位，真正的HP建议用Raw）
    TArray<FColor> DepthPixels;
    DepthPixels.SetNumUninitialized(Size.X * Size.Y);
    
    float MinDepth = TNumericLimits<float>::Max();
    float MaxDepth = TNumericLimits<float>::Lowest();
    for (float Depth : DepthValues)
    {
        if (Depth > KINDA_SMALL_NUMBER && Depth < 10000000.0f)
        {
            MinDepth = FMath::Min(MinDepth, Depth);
            MaxDepth = FMath::Max(MaxDepth, Depth);
        }
    }
    if (MinDepth > MaxDepth) { MinDepth = 0.0f; MaxDepth = 1.0f; }
    float DepthRange = FMath::Max(MaxDepth - MinDepth, KINDA_SMALL_NUMBER);

    for (int32 i = 0; i < DepthValues.Num(); i++)
    {
        float Normalized = FMath::Clamp((DepthValues[i] - MinDepth) / DepthRange, 0.0f, 1.0f);
        uint8 Gray = (uint8)(Normalized * 255.0f);
        DepthPixels[i] = FColor(Gray, Gray, Gray, 255);
    }

    return FFileHelper::CreateBitmap(*FullPath, Size.X, Size.Y, DepthPixels.GetData());
}

bool UFisheyeCameraComponent::SavePose(const FString& FilePath, int32 FrameNumber)
{
    AActor* Owner = GetOwner();
    if (!Owner) return false;
    WritePoseToFile(FilePath, Owner->GetActorLocation(), Owner->GetActorQuat(), (FrameNumber >= 0) ? FrameNumber : FrameCounter);
    return true;
}

bool UFisheyeCameraComponent::SaveAllData(const FString& BasePath, int32 FrameNumber)
{
    if (bIsCapturingSequence)
    {
        // 如果正在预热，则记录保存请求，等预热结束再保存
        PendingExportBasePath = BasePath;
        PendingFrameNumber = FrameNumber;
        UE_LOG(LogTemp, Log, TEXT("SaveAllData: Capture sequence in progress, pending save to %s"), *BasePath);
        return true;
    }

    int32 ActualFrameNumber = (FrameNumber >= 0) ? FrameNumber : FrameCounter;
    FString RootPath = BasePath;
    if (FPaths::GetCleanFilename(BasePath).StartsWith(TEXT("frame_"))) RootPath = FPaths::GetPath(BasePath);

    FString CamName = GetName();
    FString Filename = FString::Printf(TEXT("frame_%05d"), ActualFrameNumber);
    FString RGBPath = FPaths::Combine(RootPath, CamName, TEXT("rgb"), Filename);
    FString DepthPngPath = FPaths::Combine(RootPath, CamName, TEXT("depth"), Filename);
    FString DepthRawPath = FPaths::Combine(RootPath, CamName, TEXT("depth_raw"), Filename);
    FString PositionPath = FPaths::Combine(RootPath, CamName, TEXT("position"), Filename);
    FString PosePath = FPaths::Combine(RootPath, CamName, TEXT("CamState"), Filename);

    bool bRGB = SaveRGBImage(RGBPath, ActualFrameNumber);
    bool bDepthPng = SaveDepthImage(DepthPngPath, ActualFrameNumber);
    bool bDepthRaw = SaveDepthImageRaw(DepthRawPath, ActualFrameNumber);
    
    // 如果配置了位置材质，导出位置图 (EXR)
    bool bPosition = true;
    if (FisheyePositionPostProcessMaterial)
    {
        // 这里如果是基本组件，由于没有像 MultiView 那样复杂的循环，我们可能需要在 SaveAllData 内部临时触发一次 Capture
        ESceneCaptureSource OriginalSource = CaptureSource;
        UTextureRenderTarget2D* OriginalTarget = TextureTarget;
        FPostProcessSettings OriginalSettings = PostProcessSettings;

        CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
        TextureTarget = DepthRenderTarget; // 复用深度 RT 及其格式
        PostProcessSettings.WeightedBlendables.Array.Empty();
        
        UMaterialInstanceDynamic* PosMID = UMaterialInstanceDynamic::Create(FisheyePositionPostProcessMaterial, this);
        if (PosMID)
        {
            PostProcessSettings.AddBlendable(PosMID, 1.0f);
        }

        PostProcessSettings.bOverride_AutoExposureMethod = true;
        PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        PostProcessSettings.bOverride_AutoExposureBias = true;
        PostProcessSettings.AutoExposureBias = 0.0f;
        PostProcessSettings.bOverride_ToneCurveAmount = true;
        PostProcessSettings.ToneCurveAmount = 0.0f;

        CaptureScene();
        FlushRenderingCommands();

        FString PosFullPath = PositionPath + TEXT(".exr");
        FImageWriteOptions Options;
        Options.Format = EDesiredImageFormat::EXR;
        Options.bAsync = false;
        UImageWriteBlueprintLibrary::ExportToDisk(DepthRenderTarget, PosFullPath, Options);

        CaptureSource = OriginalSource;
        TextureTarget = OriginalTarget;
        PostProcessSettings = OriginalSettings;
    }

    bool bPose = SavePose(PosePath, ActualFrameNumber);

    return bRGB && bDepthPng && bDepthRaw && bPose;
}
