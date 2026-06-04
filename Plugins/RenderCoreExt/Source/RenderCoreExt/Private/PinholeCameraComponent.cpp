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

#include "PinholeCameraComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageWriteBlueprintLibrary.h"
#include "HAL/IConsoleManager.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"

UPinholeCameraComponent::UPinholeCameraComponent()
{
    // Pinhole defaults
    FisheyeParameters.AngleStep = 0.0f; // Signifies single view
    FOVAngle = 90.0f;
}

void UPinholeCameraComponent::RebuildMultiViewCaptures()
{
    ViewsMetadata.Empty();

    // Generate exactly ONE view metadata representing the current pinhole view
    FMultiViewData SingleView;
    SingleView.ViewIndex = 0;
    SingleView.Rotation = FRotator::ZeroRotator; // Use component's own rotation
    ViewsMetadata.Add(SingleView);

    // Initialize probes
    if (FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial)
    {
        if (!SharedCaptureComp)
        {
            FMultiViewProbe TempProbe;
            SetupProbe(TempProbe, 0);
            SharedCaptureComp = TempProbe.CaptureComp;
            SharedDepthRT = TempProbe.DepthRT;
            SharedRGBRT = TempProbe.RGBRT;
        }
        else
        {
            // 确保分辨率同步
            float Multiplier = FMath::Max(1.0f, FisheyeParameters.SupersamplingMultiplier);
            int32 RTWidth = FMath::RoundToInt(FisheyeParameters.Resolution.X * Multiplier);
            int32 RTHeight = FMath::RoundToInt(FisheyeParameters.Resolution.Y * Multiplier);
            if (SharedRGBRT) SharedRGBRT->ResizeTarget(RTWidth, RTHeight);
            if (SharedDepthRT) SharedDepthRT->ResizeTarget(RTWidth, RTHeight);
        }
        ProbePool.Empty();
    }
    else
    {
        // For Pinhole Parallel, we ideally want 3 probes for [RGB, Depth, Pos]
        int32 NumProbes = FMath::Clamp(FisheyeParameters.NumParallelProbes, 1, 3);
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
        if (ProbePool.Num() > 0)
        {
            SharedCaptureComp = ProbePool[0].CaptureComp;
            SharedDepthRT = ProbePool[0].DepthRT;
            SharedRGBRT = ProbePool[0].RGBRT;

            // 确保池中所有探针的分辨率同步
            float Multiplier = FMath::Max(1.0f, FisheyeParameters.SupersamplingMultiplier);
            int32 RTWidth = FMath::RoundToInt(FisheyeParameters.Resolution.X * Multiplier);
            int32 RTHeight = FMath::RoundToInt(FisheyeParameters.Resolution.Y * Multiplier);
            for (auto& Probe : ProbePool)
            {
                if (Probe.RGBRT) Probe.RGBRT->ResizeTarget(RTWidth, RTHeight);
                if (Probe.DepthRT) Probe.DepthRT->ResizeTarget(RTWidth, RTHeight);
            }
        }
    }
}

bool UPinholeCameraComponent::SaveAllData(const FString& BasePath, int32 FrameNumber)
{
    if (ViewsMetadata.Num() == 0 || !SharedCaptureComp) return false;

    int32 FrameNum = (FrameNumber >= 0) ? FrameNumber : FrameCounter;
    FString FrameFolder = FString::Printf(TEXT("%s/Frame_%04d"), *BasePath, FrameNum);
    IFileManager::Get().MakeDirectory(*FrameFolder, true);

    TArray<TSharedPtr<FJsonValue>> ViewsJsonArray;

    // Backup template settings
    FPostProcessSettings TemplatePPS = SharedCaptureComp->PostProcessSettings;
    FEngineShowFlags TemplateShowFlags = SharedCaptureComp->ShowFlags;
    ESceneCaptureSource TemplateSource = SharedCaptureComp->CaptureSource;

    static IConsoleVariable* VSMCacheCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Cache"));
    static IConsoleVariable* VSMInvalidateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.ForceInvalidate"));
    int32 OldVSMCache = VSMCacheCVar ? VSMCacheCVar->GetInt() : 1;

    FString ImageBaseName = TEXT("Pinhole_View_000");

    if (FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial || ProbePool.Num() < 2)
    {
        // Serial fallback: standard implementation from parent or simplified
        return Super::SaveAllData(BasePath, FrameNumber);
    }
    else
    {
        // Parallel mode: Map passes to different probes
        // Probe 0: RGB
        // Probe 1: Depth (if enabled)
        // Probe 2: Position (if enabled)

        if (VSMCacheCVar) VSMCacheCVar->Set(0, ECVF_SetByCode);
        if (VSMInvalidateCVar) VSMInvalidateCVar->Set(1, ECVF_SetByCode);

        // --- Step 1: Trigger all passes on different probes ---
        
        // Pass 0: RGB on Probe 0
        ProbePool[0].CaptureComp->TextureTarget = ProbePool[0].RGBRT;
        ProbePool[0].CaptureComp->PostProcessSettings = TemplatePPS;
        ProbePool[0].CaptureComp->ShowFlags = TemplateShowFlags;
        ProbePool[0].CaptureComp->ShowFlags.SetTemporalAA(false);
        ProbePool[0].CaptureComp->ShowFlags.SetMotionBlur(false);
        ProbePool[0].CaptureComp->SetWorldRotation(GetComponentRotation()); // Absolute rotation
        ProbePool[0].CaptureComp->UpdateComponentToWorld();
        ProbePool[0].CaptureComp->CaptureScene();

        // Pass 1: Depth on Probe 1
        bool bHasDepthPass = FisheyeDepthPostProcessMaterial && ProbePool.Num() > 1;
        if (bHasDepthPass)
        {
            ProbePool[1].CaptureComp->TextureTarget = ProbePool[1].DepthRT;
            ProbePool[1].CaptureComp->PostProcessSettings.WeightedBlendables.Array.Empty();
            // ProbePool[1].CaptureComp->PostProcessSettings.AddBlendable(FisheyeDepthPostProcessMaterial, 1.0f);
            ProbePool[1].CaptureComp->PostProcessSettings.AddBlendable(FisheyeDepthPostProcessMaterial, 1.0f);
            ProbePool[1].CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
            ProbePool[1].CaptureComp->ShowFlags.SetTemporalAA(false);
            ProbePool[1].CaptureComp->SetWorldRotation(GetComponentRotation());
            ProbePool[1].CaptureComp->UpdateComponentToWorld();
            ProbePool[1].CaptureComp->CaptureScene();
        }

        // Pass 2: Position on Probe 2 (or Probe 1 sequentially if only 2 probes)
        bool bHasPosPass = FisheyePositionPostProcessMaterial && ProbePool.Num() > 1;
        int32 PosProbeIdx = (ProbePool.Num() > 2) ? 2 : 1;
        
        if (bHasPosPass)
        {
            // If we only have 2 probes, we can only parallelize 2 passes. 
            // Here we prioritize RGB vs (Depth/Pos).
            // But let's assume user gives at least 3 for true parallel.
            if (PosProbeIdx != 1 || !bHasDepthPass) 
            {
                // Parallel with RGB or unique probe
                ProbePool[PosProbeIdx].CaptureComp->TextureTarget = ProbePool[PosProbeIdx].DepthRT;
                ProbePool[PosProbeIdx].CaptureComp->PostProcessSettings.WeightedBlendables.Array.Empty();
                ProbePool[PosProbeIdx].CaptureComp->PostProcessSettings.AddBlendable(FisheyePositionPostProcessMaterial, 1.0f);
                ProbePool[PosProbeIdx].CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
                ProbePool[PosProbeIdx].CaptureComp->ShowFlags.SetTemporalAA(false);
                ProbePool[PosProbeIdx].CaptureComp->SetWorldRotation(GetComponentRotation());
                ProbePool[PosProbeIdx].CaptureComp->UpdateComponentToWorld();
                ProbePool[PosProbeIdx].CaptureComp->CaptureScene();
            }
        }

        FlushRenderingCommands();

        // --- Step 2: Handle Sequential remainder and Async Export ---
        
        // Export RGB
        FString FullPathRGB = FrameFolder / (ImageBaseName + TEXT(".png"));
        FImageWriteOptions RGBOptions;
        RGBOptions.Format = EDesiredImageFormat::PNG;
        RGBOptions.bAsync = true;
        UImageWriteBlueprintLibrary::ExportToDisk(ProbePool[0].RGBRT, FullPathRGB, RGBOptions);

        // Export Depth
        if (bHasDepthPass)
        {
            FString DepthFileName = ImageBaseName + TEXT("_Depth.exr");
            FImageWriteOptions DepthOptions;
            DepthOptions.Format = EDesiredImageFormat::EXR;
            DepthOptions.bAsync = true;
            UImageWriteBlueprintLibrary::ExportToDisk(ProbePool[1].DepthRT, FrameFolder / DepthFileName, DepthOptions);
        }

        // Handle sequential Position if forced to reuse Probe 1
        if (bHasPosPass && PosProbeIdx == 1 && bHasDepthPass)
        {
            ProbePool[1].CaptureComp->PostProcessSettings.WeightedBlendables.Array.Empty();
            // ProbePool[1].CaptureComp->PostProcessSettings.AddBlendable(FisheyePositionPostProcessMaterial, 1.0f);
            ProbePool[1].CaptureComp->PostProcessSettings.AddBlendable(FisheyePositionPostProcessMaterial, 1.0f);
            ProbePool[1].CaptureComp->CaptureScene();
            FlushRenderingCommands();
        }

        // Export Position
        if (bHasPosPass)
        {
            FString PosFileName = ImageBaseName + TEXT("_Position.exr");
            FImageWriteOptions PosOptions;
            PosOptions.Format = EDesiredImageFormat::EXR;
            PosOptions.bAsync = true;
            UImageWriteBlueprintLibrary::ExportToDisk(ProbePool[PosProbeIdx].DepthRT, FrameFolder / PosFileName, PosOptions);
        }

        // --- Metadata ---
        TSharedPtr<FJsonObject> ViewObj = MakeShareable(new FJsonObject);
        ViewObj->SetNumberField(TEXT("view_index"), 0);
        ViewObj->SetStringField(TEXT("filename"), ImageBaseName + TEXT(".png"));
        if (bHasDepthPass) ViewObj->SetStringField(TEXT("depth_filename"), ImageBaseName + TEXT("_Depth.exr"));
        if (bHasPosPass) ViewObj->SetStringField(TEXT("position_filename"), ImageBaseName + TEXT("_Position.exr"));
        
        FVector Loc = GetComponentLocation();
        FQuat Rot = GetComponentQuat();
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

        ViewsJsonArray.Add(MakeShareable(new FJsonValueObject(ViewObj)));

        if (VSMCacheCVar) VSMCacheCVar->Set(OldVSMCache, ECVF_SetByCode);
    }

    // Save Summary Json
    FString JsonPath = FrameFolder / TEXT("views_metadata.json");
    TSharedPtr<FJsonObject> RootObj = MakeShareable(new FJsonObject());
    RootObj->SetArrayField(TEXT("views"), ViewsJsonArray);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    if (FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer))
    {
        FFileHelper::SaveStringToFile(JsonString, *JsonPath);
    }

    return true;
}
