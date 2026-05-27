#include "MultiViewERPCameraComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "ImageWriteBlueprintLibrary.h"
#include "HAL/IConsoleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"

// 立方体贴图 6 个面的名称，与 CubeFaceRotations 一一对应
static const TCHAR* CubeFaceNames[] =
{
    TEXT("Front"),   // +X
    TEXT("Back"),     // -X
    TEXT("Right"),    // +Y
    TEXT("Left"),     // -Y
    TEXT("Up"),       // +Z
    TEXT("Down")      // -Z
};

// 立方体贴图 6 个面的旋转 (UE 坐标系: X=前, Y=右, Z=上)
static const FRotator CubeFaceRotations[] =
{
    FRotator(0.0f,   0.0f,   0.0f),   // Front  (+X): 前方
    FRotator(0.0f,   180.0f, 0.0f),   // Back   (-X): 后方
    FRotator(0.0f,   90.0f,  0.0f),   // Right  (+Y): 右方
    FRotator(0.0f,   -90.0f, 0.0f),   // Left   (-Y): 左方
    FRotator(90.0f,  0.0f,   0.0f),   // Up     (+Z): 上方
    FRotator(-90.0f, 0.0f,   0.0f)    // Down   (-Z): 下方
};

UMultiViewERPCameraComponent::UMultiViewERPCameraComponent()
{
    // Cubemap 模式: 6 个面, 每面 90° FOV 即可完整覆盖 360x180
    FisheyeParameters.HorizontalFOV = 360.0f;
    FisheyeParameters.VerticalFOV = 180.0f;
    FisheyeParameters.AngleStep = 90.0f;
    FOVAngle = 90.0f; // 每个面恰好 90° FOV
}

void UMultiViewERPCameraComponent::BeginPlay()
{
    Super::BeginPlay();
    
    // 全局渲染优化设置 (VSM 限制等)
    if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel")))
    {
        CVar->Set(32, ECVF_SetByCode);
    }
    
    RebuildSphericalSampling();
}

void UMultiViewERPCameraComponent::ConfigureCamera(const FFisheyeParams& Params)
{
    FisheyeParameters = Params;
    
    // 自动修正 ERP 范围 (至少 360x180 以称为全景)
    if (FisheyeParameters.HorizontalFOV < 359.0f) FisheyeParameters.HorizontalFOV = 360.0f;
    if (FisheyeParameters.VerticalFOV < 179.0f) FisheyeParameters.VerticalFOV = 180.0f;

    RebuildSphericalSampling();

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
        // SCS_FinalColorLDR: 完整 Tonemapping + Gamma，适合 8-bit PNG 输出
        CapComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        CapComp->bAlwaysPersistRenderingState = true;
        
        // 应用后处理设置 (关键：禁用屏幕追踪)
        CapComp->PostProcessSettings = this->PostProcessSettings;
        CapComp->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
        CapComp->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
        
        CapComp->PostProcessSettings.bOverride_LumenFinalGatherScreenTraces = true;
        CapComp->PostProcessSettings.LumenFinalGatherScreenTraces = false;
        CapComp->PostProcessSettings.bOverride_LumenReflectionsScreenTraces = true;
        CapComp->PostProcessSettings.LumenReflectionsScreenTraces = false;

        // 3. 强制固定曝光 (解决 ERP 过曝/闪烁的关键)
        CapComp->PostProcessSettings.bOverride_AutoExposureMethod = true;
        CapComp->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        CapComp->PostProcessSettings.bOverride_AutoExposureBias = true;
        CapComp->PostProcessSettings.AutoExposureBias = Params.ExposureBias;

        // Lumen 设置优化
        CapComp->PostProcessSettings.bOverride_LumenSceneLightingQuality = true;
        CapComp->PostProcessSettings.LumenSceneLightingQuality = 1.0f;
        
        // 4. ShowFlags 全开以确保高质量渲染
        CapComp->ShowFlags = this->ShowFlags;
        CapComp->ShowFlags.SetLumenGlobalIllumination(true);
        CapComp->ShowFlags.SetLumenReflections(true);
        CapComp->ShowFlags.SetGlobalIllumination(true);
        CapComp->ShowFlags.SetIndirectLightingCache(true);
        CapComp->ShowFlags.SetLighting(true);
        CapComp->ShowFlags.SetSkyLighting(true);
        CapComp->ShowFlags.SetPostProcessing(true);

        CapComp->ShowFlags.SetScreenSpaceAO(false);
        CapComp->ShowFlags.SetContactShadows(false);
        CapComp->ShowFlags.SetScreenSpaceReflections(false);
        CapComp->ShowFlags.SetDynamicShadows(FisheyeParameters.bEnableShadows);

        // 5. Tone Curve / Gamma
        CapComp->PostProcessSettings.bOverride_ToneCurveAmount = true;
        CapComp->PostProcessSettings.ToneCurveAmount = Params.bEnableToneCurve ? 1.0f : 0.0f;

        // 设置分辨率
        float Multiplier = FMath::Max(1.0f, Params.SupersamplingMultiplier);
        int32 RTWidth = FMath::RoundToInt(Params.Resolution.X * Multiplier);
        int32 RTHeight = FMath::RoundToInt(Params.Resolution.Y * Multiplier);
        
        if (CapComp->TextureTarget)
        {
            CapComp->TextureTarget->InitCustomFormat(RTWidth, RTHeight, PF_B8G8R8A8, false);
            CapComp->TextureTarget->UpdateResourceImmediate(true);
        }
    }
}

void UMultiViewERPCameraComponent::SetupProbe(FMultiViewProbe& OutProbe, int32 Index)
{
    FString CompName = FString::Printf(TEXT("ERP_Probe_%d"), Index);
    OutProbe.CaptureComp = NewObject<USceneCaptureComponent2D>(this, *CompName);
    OutProbe.CaptureComp->RegisterComponent();
    OutProbe.CaptureComp->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
    
    OutProbe.CaptureComp->TextureTarget = NewObject<UTextureRenderTarget2D>(OutProbe.CaptureComp);
    OutProbe.CaptureComp->TextureTarget->InitCustomFormat(1024, 1024, PF_B8G8R8A8, false);
    OutProbe.CaptureComp->TextureTarget->UpdateResourceImmediate(true);
    
    OutProbe.CaptureComp->bCaptureEveryFrame = false;
    OutProbe.CaptureComp->bCaptureOnMovement = false;
    OutProbe.CaptureComp->bAlwaysPersistRenderingState = true;
    // SCS_FinalColorLDR: 包含完整 Tonemapping + Gamma 矫正，适合 8-bit PNG 输出
    // SCS_FinalColorHDR 输出线性 HDR 值存入 8-bit RT 会被截断导致图像偏暗
    OutProbe.CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

    OutProbe.DepthRT = NewObject<UTextureRenderTarget2D>(this);
    OutProbe.DepthRT->InitCustomFormat(1024, 1024, PF_A32B32G32R32F, false);
    OutProbe.DepthRT->UpdateResourceImmediate(true);
}

void UMultiViewERPCameraComponent::RebuildSphericalSampling()
{
    ViewsMetadata.Empty();

    if (FisheyeParameters.RenderMode == EMultiViewRenderMode::Serial)
    {
        if (!SharedCaptureComp)
        {
            FMultiViewProbe TempProbe;
            SetupProbe(TempProbe, 0);
            SharedCaptureComp = TempProbe.CaptureComp;
            SharedDepthRT = TempProbe.DepthRT;
        }
        ProbePool.Empty();
    }
    else
    {
        int32 NumProbes = FMath::Max(1, FisheyeParameters.NumParallelProbes);
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
        }
    }

    // --- 立方体贴图 6 面采样 (Cubemap 6-Face) ---
    // 仅渲染 6 个方向 (Front/Back/Right/Left/Up/Down)，
    // 每面 90° FOV，恰好无重叠无缝隙地覆盖完整 360°×180° 球面。
    // 后续可在 CPU/GPU 上将 6 张图拼接为 Equirectangular 全景图。
    for (int32 i = 0; i < 6; i++)
    {
        FMultiViewData Meta;
        Meta.ViewIndex = i;
        Meta.Rotation = CubeFaceRotations[i];
        ViewsMetadata.Add(Meta);
    }

    UE_LOG(LogTemp, Log, TEXT("[ERP] Cubemap mode: 6 faces (FOV=90). Total views = %d."), ViewsMetadata.Num());
}

void UMultiViewERPCameraComponent::CaptureFisheyeScene()
{
    UpdateComponentToWorld();
    
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
        CapComp->SetRelativeRotation(FRotator::ZeroRotator);
        CapComp->UpdateComponentToWorld();
        CapComp->CaptureScene();
    }
    FlushRenderingCommands();
}

bool UMultiViewERPCameraComponent::SaveAllData(const FString& BasePath, int32 FrameNumber)
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

    // Lumen warmup 相关 CVar (只查找一次)
    static IConsoleVariable* LumenAllowCVar   = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.DiffuseIndirect.Allow"));
    static IConsoleVariable* LumenTracingCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.Reflections.Allow"));
    
    // --- 【新增】：全局预热阶段 (Global Warmup) ---
    // 所有探针都在同一个世界位置，Lumen Surface Cache 和 VSM 只需要预热一次。
    // 我们强制使用第 0 个探针进行预热，防止 VSM 页池被多个探针重复 invalidation 撑爆。
    if (NumProbes > 0 && FisheyeParameters.WarmUpFrames > 0)
    {
        FMultiViewProbe& WarmupProbe = ActiveProbes[0];
        // 临时提高预热和最终捕获的质量，因为紧凑循环没有时间流逝，无法通过 TAA 收敛
        float OldLightingQuality = WarmupProbe.CaptureComp->PostProcessSettings.LumenSceneLightingQuality;
        WarmupProbe.CaptureComp->PostProcessSettings.bOverride_LumenSceneLightingQuality = true;
        WarmupProbe.CaptureComp->PostProcessSettings.LumenSceneLightingQuality = 4.0f; // 强拉空间质量

        for (int32 w = 0; w < FisheyeParameters.WarmUpFrames; w++)
        {
            if (FisheyeParameters.bEnableShadows)
            {
                // 只在第一帧 Invalidate 历史阴影，后续帧让其缓存
                if (VSMCacheCVar) VSMCacheCVar->Set(w == 0 ? 0 : 1, ECVF_SetByCode);
                if (VSMInvalidateCVar) VSMInvalidateCVar->Set(w == 0 ? 1 : 0, ECVF_SetByCode);
            }
            WarmupProbe.CaptureComp->CaptureScene();
            FlushRenderingCommands();
        }
        
        // 预热结束后，确保 VSM 缓存开启，供后续所有探针共用
        if (FisheyeParameters.bEnableShadows)
        {
            if (VSMCacheCVar) VSMCacheCVar->Set(1, ECVF_SetByCode);
            if (VSMInvalidateCVar) VSMInvalidateCVar->Set(0, ECVF_SetByCode);
        }
        
        // 恢复所有探针的渲染质量 (可选，这里保持高质量直到当前帧捕获完毕)
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
            // 取消每次探针渲染时的 Invalidate，直接复用预热好的 VSM Cache
            Probe.CaptureComp->CaptureScene();
        }

        // 等待当前 Batch 渲染完成
        FlushRenderingCommands();
        // --- 第二步：提取结果并保存 ---
        for (int32 j = 0; j < CurrentBatchSize; j++)
        {
            int32 ViewIdx = i + j;
            FMultiViewProbe& Probe = ActiveProbes[j];
            const FMultiViewData& Meta = ViewsMetadata[ViewIdx];
            // 使用 CubeFace 名称命名 (Front, Back, Right, Left, Up, Down)
            const TCHAR* FaceName = (Meta.ViewIndex >= 0 && Meta.ViewIndex < 6) ? CubeFaceNames[Meta.ViewIndex] : TEXT("Unknown");
            FString ImageBaseName = FString::Printf(TEXT("CubeFace_%d_%s"), Meta.ViewIndex, FaceName);

            // RGB 导出
            FString FullPathRGB = FrameFolder / (ImageBaseName + TEXT(".png"));
            FImageWriteOptions RGBOptions;
            RGBOptions.Format = EDesiredImageFormat::PNG;
            RGBOptions.bAsync = false;
            UImageWriteBlueprintLibrary::ExportToDisk(Probe.CaptureComp->TextureTarget, FullPathRGB, RGBOptions);

            // Metadata
            TSharedPtr<FJsonObject> ViewObj = MakeShareable(new FJsonObject);
            ViewObj->SetNumberField(TEXT("view_index"), Meta.ViewIndex);
            ViewObj->SetStringField(TEXT("face_name"), FaceName);
            ViewObj->SetNumberField(TEXT("pitch"), Meta.Rotation.Pitch);
            ViewObj->SetNumberField(TEXT("yaw"), Meta.Rotation.Yaw);
            ViewObj->SetStringField(TEXT("filename"), ImageBaseName + TEXT(".png"));
            ViewObj->SetNumberField(TEXT("width"), Probe.CaptureComp->TextureTarget->SizeX);
            ViewObj->SetNumberField(TEXT("height"), Probe.CaptureComp->TextureTarget->SizeY);
            ViewObj->SetNumberField(TEXT("fov"), Probe.CaptureComp->FOVAngle);

            FVector Loc = Probe.CaptureComp->GetComponentLocation();
            TArray<TSharedPtr<FJsonValue>> PosArr;
            PosArr.Add(MakeShareable(new FJsonValueNumber(Loc.X)));
            PosArr.Add(MakeShareable(new FJsonValueNumber(Loc.Y)));
            PosArr.Add(MakeShareable(new FJsonValueNumber(Loc.Z)));
            ViewObj->SetArrayField(TEXT("world_pos"), PosArr);

            FQuat Rot = Probe.CaptureComp->GetComponentQuat();
            TArray<TSharedPtr<FJsonValue>> RotArr;
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.X)));
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.Y)));
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.Z)));
            RotArr.Add(MakeShareable(new FJsonValueNumber(Rot.W)));
            ViewObj->SetArrayField(TEXT("world_rot_quat"), RotArr);

            // Depth 导出
            if (FisheyeDepthPostProcessMaterial && Probe.DepthRT)
            {
                UTextureRenderTarget2D* OriginalRT = Probe.CaptureComp->TextureTarget;
                FPostProcessSettings OriginalPPSettings = Probe.CaptureComp->PostProcessSettings;
                ESceneCaptureSource OriginalSource = Probe.CaptureComp->CaptureSource;
                FEngineShowFlags OriginalShowFlags = Probe.CaptureComp->ShowFlags;

                Probe.CaptureComp->TextureTarget = Probe.DepthRT;
                Probe.CaptureComp->PostProcessSettings.WeightedBlendables.Array.Empty();
                // Probe.CaptureComp->PostProcessSettings.AddBlendable(FisheyeDepthPostProcessMaterial, 1.0f);
                Probe.CaptureComp->PostProcessSettings.AddBlendable(FisheyeDepthPostProcessMaterial, 1.0f);
                Probe.CaptureComp->ShowFlags.SetTemporalAA(false);
                Probe.CaptureComp->ShowFlags.SetAntiAliasing(false);
                Probe.CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;

                Probe.CaptureComp->CaptureScene();
                FlushRenderingCommands();

                FString DepthName = ImageBaseName + TEXT("_Depth.exr");
                FImageWriteOptions DepthOptions;
                DepthOptions.Format = EDesiredImageFormat::EXR;
                DepthOptions.bAsync = false;
                UImageWriteBlueprintLibrary::ExportToDisk(Probe.DepthRT, FrameFolder / DepthName, DepthOptions);
                ViewObj->SetStringField(TEXT("depth_filename"), DepthName);

                Probe.CaptureComp->TextureTarget = OriginalRT;
                Probe.CaptureComp->PostProcessSettings = OriginalPPSettings;
                Probe.CaptureComp->CaptureSource = OriginalSource;
                Probe.CaptureComp->ShowFlags = OriginalShowFlags;
            }

            // Position 导出
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

                FString PosName = ImageBaseName + TEXT("_Position.exr");
                FImageWriteOptions PosOptions;
                PosOptions.Format = EDesiredImageFormat::EXR;
                PosOptions.bAsync = false;
                UImageWriteBlueprintLibrary::ExportToDisk(Probe.DepthRT, FrameFolder / PosName, PosOptions);
                ViewObj->SetStringField(TEXT("position_filename"), PosName);

                Probe.CaptureComp->TextureTarget = OriginalRT;
                Probe.CaptureComp->PostProcessSettings = OriginalPPSettings;
                Probe.CaptureComp->CaptureSource = OriginalSource;
                Probe.CaptureComp->ShowFlags = OriginalShowFlags;
            }

            ViewsJsonArray[ViewIdx] = MakeShareable(new FJsonValueObject(ViewObj));
        }

        if (FisheyeParameters.bEnableShadows && VSMCacheCVar) VSMCacheCVar->Set(OldVSMCache, ECVF_SetByCode);
    }

    // Summary Json
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
    
    // Cubemap 拼接元数据
    RootObj->SetStringField(TEXT("projection_mode"), TEXT("cubemap_6face"));
    RootObj->SetNumberField(TEXT("num_faces"), 6);
    RootObj->SetNumberField(TEXT("face_fov"), 90.0f);
    
    TArray<TSharedPtr<FJsonValue>> FaceOrderArr;
    for (int32 fi = 0; fi < 6; fi++)
    {
        FaceOrderArr.Add(MakeShareable(new FJsonValueString(CubeFaceNames[fi])));
    }
    RootObj->SetArrayField(TEXT("face_order"), FaceOrderArr);
    
    RootObj->SetArrayField(TEXT("views"), ViewsJsonArray);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    if (FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer))
    {
        FFileHelper::SaveStringToFile(JsonString, *JsonPath);
    }

    return true;
}
