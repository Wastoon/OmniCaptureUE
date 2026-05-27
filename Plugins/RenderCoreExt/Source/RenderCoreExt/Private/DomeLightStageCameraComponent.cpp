#include "DomeLightStageCameraComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "ImageWriteBlueprintLibrary.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Components/SkinnedMeshComponent.h"  
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
// =============================================================================
// 骨骼语义映射表
//
// 设计原则：
//   1. 精确匹配（Equals）优先，覆盖 FACIAL_C_Forehead 等主控骨骼
//   2. 前缀匹配（StartsWith）次之，覆盖 FACIAL_L_Forehead1/2/3 等子骨骼
//   3. 包含匹配（Contains）兜底，覆盖 FACIAL_L_12IPV_ForeheadIn* 等插值骨骼
//
// MetaHuman 骨骼命名规律（来自真实骨骼树）：
//   FACIAL_[C/L/R]_<区域名>[数字]      主控/辅助骨骼
//   FACIAL_[C/L/R]_12IPV_<区域名>[数字] 12-point Interpolation Volume（插值骨骼）
//   这两类都承担大量蒙皮权重，必须都覆盖到。
//
// 每个 entry 的 BoneKeyword 会被用于：Equals → StartsWith → Contains 三轮匹配。
// =============================================================================

namespace
{

struct FBoneSemanticEntry
{
    const TCHAR* BoneKeyword; // 用于三轮匹配的关键词
    FLinearColor  Color;
};

// ── 颜色约定（尽量让相邻区域颜色对比度大，便于分割） ──────────────────────
// 面部区域用暖色系，身体用冷色系，手指用黄绿梯度，腿脚用蓝紫梯度
static const FBoneSemanticEntry GBoneSemanticMap[] =
{
    //─── 面部根 / 通用面部（最低优先级，精确匹配 FacialRoot）────────────────
    { TEXT("FACIAL_C_FacialRoot"),      FLinearColor(0.90f, 0.10f, 0.10f, 1.f) }, // 深红，Face 兜底

    //─── 额头 ────────────────────────────────────────────────────────────────
    // 关键词 "Forehead" 可以通过 Contains 覆盖：
    //   FACIAL_C_Forehead / FACIAL_L_Forehead1-3 / FACIAL_R_Forehead1-3
    //   FACIAL_L_12IPV_ForeheadIn* / FACIAL_L_12IPV_ForeheadMid* / ForeheadOut*
    //   FACIAL_L_ForeheadInSkin / ForeheadMidSkin / ForeheadOutSkin
    { TEXT("Forehead"),                 FLinearColor(1.00f, 0.30f, 0.10f, 1.f) }, // 橙红

    //─── 眼睛 / 眼睑 / 眼袋 ──────────────────────────────────────────────────
    // 覆盖：FACIAL_L/R_Eye / EyeParallel / Pupil
    //        EyelidUpperA/B / EyelidLowerA/B / EyelidUpperFurrow
    //        EyeCornerInner / EyeCornerOuter
    //        EyesackUpper / EyesackLower / 12IPV_EyesackL*
    //        EyelashesUpperA* / EyelashesCornerOuter*
    { TEXT("Eye"),                      FLinearColor(0.00f, 1.00f, 1.00f, 1.f) }, // 青色
    { TEXT("Eyelid"),                   FLinearColor(0.10f, 0.90f, 0.85f, 1.f) }, // 浅青
    { TEXT("Eyelash"),                  FLinearColor(0.05f, 0.70f, 0.70f, 1.f) }, // 暗青（睫毛）
    { TEXT("Eyesack"),                  FLinearColor(0.15f, 0.80f, 0.80f, 1.f) }, // 眼袋
    { TEXT("Pupil"),                    FLinearColor(0.00f, 1.00f, 1.00f, 1.f) }, // 同眼睛色

    //─── 鼻子 ─────────────────────────────────────────────────────────────────
    // 覆盖：FACIAL_C_Nose / NoseLower / NoseTip / NoseBridge / NoseUpper
    //        FACIAL_L/R_Nostril / NostrilThickness* / 12IPV_Nostril*
    //        FACIAL_L/R_NoseBridge / NoseUpper / 12IPV_NoseUpper* / NoseBridge*
    { TEXT("Nose"),                     FLinearColor(1.00f, 0.00f, 0.80f, 1.f) }, // 品红
    { TEXT("Nostril"),                  FLinearColor(0.90f, 0.00f, 0.70f, 1.f) }, // 暗品红

    //─── 嘴唇 / 嘴角 ─────────────────────────────────────────────────────────
    // 覆盖：FACIAL_C/L/R_LipUpper / LipLower / LipCorner
    //        LipUpperOuter / LipLowerOuter / LipUpperSkin / LipLowerSkin
    //        LipUpperOuterSkin / LipLowerOuterSkin
    //        12IPV_LipUpper* / 12IPV_LipLower* / 12IPV_LipUpperSkin / LipLowerSkin
    //        MouthUpper / MouthLower / MouthInteriorUpper* / MouthInteriorLower*
    { TEXT("LipUpper"),                 FLinearColor(1.00f, 0.10f, 0.30f, 1.f) }, // 玫红，上嘴唇
    { TEXT("LipLower"),                 FLinearColor(0.75f, 0.00f, 0.25f, 1.f) }, // 暗红，下嘴唇
    { TEXT("LipCorner"),                FLinearColor(1.00f, 0.20f, 0.40f, 1.f) }, // 嘴角
    { TEXT("MouthUpper"),               FLinearColor(1.00f, 0.10f, 0.30f, 1.f) }, // 同上嘴唇
    { TEXT("MouthLower"),               FLinearColor(0.75f, 0.00f, 0.25f, 1.f) }, // 同下嘴唇
    { TEXT("MouthInterior"),            FLinearColor(0.60f, 0.00f, 0.20f, 1.f) }, // 口腔内侧
    { TEXT("LipUpperSkin"),             FLinearColor(1.00f, 0.10f, 0.30f, 1.f) },
    { TEXT("LipLowerSkin"),             FLinearColor(0.75f, 0.00f, 0.25f, 1.f) },
    { TEXT("LowerLipRotation"),         FLinearColor(0.75f, 0.00f, 0.25f, 1.f) },

    //─── 牙齿 / 舌头 ─────────────────────────────────────────────────────────
    { TEXT("Teeth"),                    FLinearColor(1.00f, 1.00f, 1.00f, 1.f) }, // 白
    { TEXT("Tongue"),                   FLinearColor(0.90f, 0.40f, 0.40f, 1.f) }, // 肉色

    //─── 面颊（Cheek）────────────────────────────────────────────────────────
    // 覆盖：CheekInner / CheekLower / CheekOuter / 12IPV_CheekOuter* / 12IPV_CheekL*
    { TEXT("Cheek"),                    FLinearColor(1.00f, 0.70f, 0.20f, 1.f) }, // 橘黄

    //─── 下颌 / 下巴 ─────────────────────────────────────────────────────────
    // 覆盖：FACIAL_C_Jaw / FACIAL_L/R_Chin / Jawline / JawBulge / JawRecess
    //        12IPV_Chin* / 12IPV_Jawline*
    { TEXT("Jaw"),                      FLinearColor(0.65f, 0.30f, 0.10f, 1.f) }, // 棕
    { TEXT("Chin"),                     FLinearColor(0.75f, 0.38f, 0.15f, 1.f) }, // 浅棕
    { TEXT("Jawline"),                  FLinearColor(0.60f, 0.25f, 0.08f, 1.f) }, // 深棕

    //─── 耳朵 ─────────────────────────────────────────────────────────────────
    { TEXT("Ear"),                      FLinearColor(0.20f, 0.55f, 1.00f, 1.f) }, // 蓝

    //─── 鬓角 / 头发附着区 ───────────────────────────────────────────────────
    // 覆盖：FACIAL_L/R_Sideburn* / FACIAL_C/L/R_Hair*
    { TEXT("Sideburn"),                 FLinearColor(0.60f, 0.30f, 0.80f, 1.f) }, // 紫
    { TEXT("FACIAL_C_Hair"),            FLinearColor(0.55f, 0.25f, 0.75f, 1.f) }, // 紫（发际）
    { TEXT("FACIAL_L_Hair"),            FLinearColor(0.55f, 0.25f, 0.75f, 1.f) },
    { TEXT("FACIAL_R_Hair"),            FLinearColor(0.55f, 0.25f, 0.75f, 1.f) },

    //─── 颧骨 / 鼻唇沟 / 太阳穴 ─────────────────────────────────────────────
    // 覆盖：NasolabialBulge / NasolabialFurrow / 12IPV_NasolabialB* / NasolabialF*
    //        Temple / 12IPV_Temple* / Masseter
    { TEXT("Nasolabial"),               FLinearColor(1.00f, 0.55f, 0.00f, 1.f) }, // 橙，鼻唇沟
    { TEXT("Temple"),                   FLinearColor(1.00f, 0.65f, 0.15f, 1.f) }, // 浅橙，太阳穴
    { TEXT("Masseter"),                 FLinearColor(0.95f, 0.60f, 0.15f, 1.f) }, // 咬肌

    //─── 颅骨 / 头皮区 ────────────────────────────────────────────────────────
    { TEXT("Skull"),                    FLinearColor(0.85f, 0.10f, 0.10f, 1.f) }, // 归Face色
    { TEXT("ForeheadSkin"),             FLinearColor(1.00f, 0.30f, 0.10f, 1.f) }, // 归额头色

    //─── 下巴下方 ─────────────────────────────────────────────────────────────
    // 覆盖：UnderChin / 12IPV_UnderChin*
    { TEXT("UnderChin"),                FLinearColor(0.50f, 0.20f, 0.05f, 1.f) }, // 脖颈交界

    //─── 颈部（FACIAL_ 骨骼命名的颈部，与 body 的 neck 区分）────────────────
    // 覆盖：FACIAL_C/L/R_NeckA*/NeckB* / NeckBack* / AdamsApple
    //        12IPV_NeckA* / 12IPV_NeckB* / 12IPV_NeckBack*
    { TEXT("FACIAL_C_Neck"),            FLinearColor(0.35f, 0.75f, 0.25f, 1.f) }, // 草绿，颈前
    { TEXT("FACIAL_L_Neck"),            FLinearColor(0.30f, 0.65f, 0.22f, 1.f) },
    { TEXT("FACIAL_R_Neck"),            FLinearColor(0.30f, 0.65f, 0.22f, 1.f) },
    { TEXT("NeckBack"),                 FLinearColor(0.25f, 0.60f, 0.20f, 1.f) }, // 颈后
    { TEXT("AdamsApple"),               FLinearColor(0.35f, 0.70f, 0.22f, 1.f) },
    { TEXT("12IPV_Neck"),               FLinearColor(0.32f, 0.68f, 0.22f, 1.f) }, // 插值骨骼颈部
    { TEXT("Neck1Root"),                FLinearColor(0.35f, 0.75f, 0.25f, 1.f) },
    { TEXT("Neck2Root"),                FLinearColor(0.35f, 0.75f, 0.25f, 1.f) },

    //─── Body 骨骼（UE Mannequin 命名）────────────────────────────────────────
    // head / neck 骨骼（body skeleton 里的，区别于 FACIAL_ 前缀）
    { TEXT("head"),                     FLinearColor(0.85f, 0.10f, 0.10f, 1.f) }, // 归 Face
    { TEXT("neck_01"),                  FLinearColor(0.35f, 0.75f, 0.25f, 1.f) }, // 颈部
    { TEXT("neck_02"),                  FLinearColor(0.35f, 0.75f, 0.25f, 1.f) },

    // 躯干
    { TEXT("spine_05"),                 FLinearColor(0.00f, 0.85f, 0.30f, 1.f) }, // 上胸
    { TEXT("spine_04"),                 FLinearColor(0.00f, 0.80f, 0.28f, 1.f) },
    { TEXT("spine_03"),                 FLinearColor(0.00f, 0.75f, 0.25f, 1.f) }, // 胸背
    { TEXT("spine_02"),                 FLinearColor(0.00f, 0.70f, 0.23f, 1.f) }, // 腰
    { TEXT("spine_01"),                 FLinearColor(0.00f, 0.65f, 0.20f, 1.f) }, // 下腰
    { TEXT("pelvis"),                   FLinearColor(0.00f, 0.55f, 0.30f, 1.f) }, // 骨盆

    // 锁骨 / 肩胛
    { TEXT("clavicle_l"),               FLinearColor(0.90f, 0.90f, 0.00f, 1.f) }, // 黄，左锁骨
    { TEXT("clavicle_scap_l"),          FLinearColor(0.85f, 0.85f, 0.00f, 1.f) },
    { TEXT("clavicle_out_l"),           FLinearColor(0.80f, 0.80f, 0.00f, 1.f) },
    { TEXT("clavicle_pec_l"),           FLinearColor(0.80f, 0.80f, 0.00f, 1.f) },
    { TEXT("clavicle_r"),               FLinearColor(0.70f, 1.00f, 0.00f, 1.f) }, // 黄绿，右锁骨
    { TEXT("clavicle_scap_r"),          FLinearColor(0.65f, 0.95f, 0.00f, 1.f) },
    { TEXT("clavicle_out_r"),           FLinearColor(0.60f, 0.90f, 0.00f, 1.f) },
    { TEXT("clavicle_pec_r"),           FLinearColor(0.60f, 0.90f, 0.00f, 1.f) },

    // 左臂（各校正骨骼用同一区域色）
    { TEXT("upperarm_l"),               FLinearColor(0.95f, 0.85f, 0.00f, 1.f) }, // 深黄，左上臂
    { TEXT("lowerarm_l"),               FLinearColor(0.90f, 0.70f, 0.00f, 1.f) }, // 橙黄，左前臂
    { TEXT("hand_l"),                   FLinearColor(1.00f, 0.50f, 0.00f, 1.f) }, // 橙，左手掌

    // 右臂
    { TEXT("upperarm_r"),               FLinearColor(0.60f, 0.90f, 0.00f, 1.f) }, // 黄绿，右上臂
    { TEXT("lowerarm_r"),               FLinearColor(0.50f, 0.80f, 0.00f, 1.f) }, // 绿，右前臂
    { TEXT("hand_r"),                   FLinearColor(0.40f, 0.70f, 0.00f, 1.f) }, // 深绿，右手掌

    // 手指（前缀匹配覆盖 _01_l / _02_l / _03_l 三节指骨，左右同色）
    { TEXT("thumb_"),                   FLinearColor(1.00f, 0.35f, 0.00f, 1.f) }, // 橙红，拇指
    { TEXT("index_"),                   FLinearColor(1.00f, 0.55f, 0.10f, 1.f) }, // 橙，食指
    { TEXT("middle_"),                  FLinearColor(1.00f, 0.70f, 0.15f, 1.f) }, // 金，中指
    { TEXT("ring_"),                    FLinearColor(1.00f, 0.85f, 0.20f, 1.f) }, // 黄，无名指
    { TEXT("pinky_"),                   FLinearColor(1.00f, 1.00f, 0.30f, 1.f) }, // 亮黄，小指

    // 腿部
    { TEXT("thigh_l"),                  FLinearColor(0.00f, 0.20f, 1.00f, 1.f) }, // 蓝，左大腿
    { TEXT("calf_l"),                   FLinearColor(0.00f, 0.40f, 1.00f, 1.f) }, // 浅蓝，左小腿
    { TEXT("foot_l"),                   FLinearColor(0.50f, 0.00f, 1.00f, 1.f) }, // 紫，左脚
    { TEXT("ball_l"),                   FLinearColor(0.60f, 0.00f, 1.00f, 1.f) }, // 紫，左脚掌
    { TEXT("thigh_r"),                  FLinearColor(0.00f, 0.45f, 0.90f, 1.f) }, // 深蓝，右大腿
    { TEXT("calf_r"),                   FLinearColor(0.00f, 0.60f, 0.80f, 1.f) }, // 青蓝，右小腿
    { TEXT("foot_r"),                   FLinearColor(0.35f, 0.00f, 0.90f, 1.f) }, // 蓝紫，右脚
    { TEXT("ball_r"),                   FLinearColor(0.45f, 0.00f, 0.85f, 1.f) }, // 蓝紫，右脚掌

    // latissimus（背阔肌校正骨骼）→ 归躯干
    { TEXT("latissimus"),               FLinearColor(0.00f, 0.75f, 0.25f, 1.f) },
};

// 未匹配到任何骨骼时的默认色（亮品红，便于发现未覆盖骨骼）
constexpr FLinearColor GSemanticDefaultColor(1.0f, 0.5f, 1.0f, 1.0f);

} // namespace

// =============================================================================
// UDomeLightStageCameraComponent
// =============================================================================

UDomeLightStageCameraComponent::UDomeLightStageCameraComponent()
{
    FOVAngle = 50.0f;
}

void UDomeLightStageCameraComponent::BeginPlay()
{
    Super::BeginPlay();
    auto SetCVar = [](const TCHAR* Name, int32 Val)
    {
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
            CVar->Set(Val, ECVF_SetByCode);
    };
    SetCVar(TEXT("r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel"), 32);
    SetCVar(TEXT("r.LumenScene.SurfaceCache.CaptureLighting"), 1);
    SetCVar(TEXT("r.Lumen.TraceMeshSDFs"), 1);
    SetCVar(TEXT("r.Lumen.DiffuseIndirect.Allow"), 1);
    SetCVar(TEXT("r.Lumen.Reflections.Allow"), 1);
    SetCVar(TEXT("r.AntiAliasingMethod"), 2);
    SetCVar(TEXT("r.TemporalAA.Upsampling"), 1);
    SetCVar(TEXT("r.TemporalAA.Algorithm"), 1);
    SetCVar(TEXT("r.GpuProfilerMaxEventBufferSizeKB"), 256);
}

// ─── ConfigureDome ────────────────────────────────────────────────────────────

void UDomeLightStageCameraComponent::ConfigureDome(const FDomeCameraConfig& Config)
{
    DomeConfig = Config;
    BuildDomeCameras();
    RebuildProbePool();

    for (USceneCaptureComponent2D* Cap : ProbeCaptures)
    {
        if (!Cap) continue;
        Cap->FOVAngle = DomeConfig.CameraFOV;
        Cap->bCaptureEveryFrame = false;
        Cap->bCaptureOnMovement = false;
        Cap->bAlwaysPersistRenderingState = true;
        Cap->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

        Cap->ShowFlags.SetLumenGlobalIllumination(true);
        Cap->ShowFlags.SetLumenReflections(true);
        Cap->ShowFlags.SetGlobalIllumination(true);
        Cap->ShowFlags.SetLighting(true);
        Cap->ShowFlags.SetSkyLighting(true);
        Cap->ShowFlags.SetPostProcessing(true);
        Cap->ShowFlags.SetDynamicShadows(DomeConfig.bEnableShadows);
        Cap->ShowFlags.SetContactShadows(true);
        Cap->ShowFlags.SetScreenSpaceReflections(false);
        Cap->ShowFlags.SetScreenSpaceAO(false);
        Cap->ShowFlags.SetTemporalAA(true);
        Cap->ShowFlags.SetAntiAliasing(true);
        Cap->ShowFlags.SetMotionBlur(false);
        Cap->ShowFlags.SetBloom(false);
        Cap->ShowFlags.SetEyeAdaptation(false);
        Cap->ShowFlags.SetGrain(false);
        Cap->ShowFlags.SetVignette(false);
        Cap->ShowFlags.SetLensFlares(false);
        Cap->ShowFlags.SetDepthOfField(false);

        FPostProcessSettings& PPS = Cap->PostProcessSettings;
        PPS.bOverride_AutoExposureMethod = true;
        PPS.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        PPS.bOverride_AutoExposureBias = true;
        PPS.AutoExposureBias = DomeConfig.ExposureBias;
        PPS.bOverride_ToneCurveAmount = true;
        PPS.ToneCurveAmount = DomeConfig.bEnableToneCurve ? 1.0f : 0.0f;
        PPS.bOverride_DynamicGlobalIlluminationMethod = true;
        PPS.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
        PPS.bOverride_LumenSceneLightingQuality = true;
        PPS.LumenSceneLightingQuality = 4.0f;
        PPS.bOverride_LumenFinalGatherQuality = true;
        PPS.LumenFinalGatherQuality = 4.0f;
        PPS.bOverride_LumenFinalGatherScreenTraces = true;
        PPS.LumenFinalGatherScreenTraces = false;
        PPS.bOverride_LumenMaxTraceDistance = true;
        PPS.LumenMaxTraceDistance = 100000.0f;
        PPS.bOverride_LumenReflectionQuality = true;
        PPS.LumenReflectionQuality = 4.0f;
        PPS.bOverride_LumenReflectionsScreenTraces = true;
        PPS.LumenReflectionsScreenTraces = false;
        PPS.bOverride_FilmGrainIntensity = true;
        PPS.FilmGrainIntensity = 0.0f;
        PPS.bOverride_SceneFringeIntensity = true;
        PPS.SceneFringeIntensity = 0.0f;

        if (Cap->TextureTarget)
        {
            Cap->TextureTarget->InitCustomFormat(
                DomeConfig.Resolution.X, DomeConfig.Resolution.Y, PF_B8G8R8A8, false);
            Cap->TextureTarget->UpdateResourceImmediate(true);
        }
    }

    auto LoadMat = [](const FString& Path, UMaterialInterface*& OutMat)
    {
        if (!Path.IsEmpty())
            if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Path))
                OutMat = M;
    };
    LoadMat(Config.DepthMaterialPath,    FisheyeDepthPostProcessMaterial);
    LoadMat(Config.NormalMaterialPath,   FisheyeNormalPostProcessMaterial);
    LoadMat(Config.PositionMaterialPath, FisheyePositionPostProcessMaterial);
    LoadMat(Config.SemanticMaterialPath, FisheyeSemanticPostProcessMaterial);

    BuildSemanticCacheForScene();

    UE_LOG(LogTemp, Warning,
        TEXT("[DomeLightStage] Configured: %d cameras, SemanticCache=%d"),
        DomeCameras.Num(), SemanticCache.Num());
}

// ─── BuildDomeCameras ────────────────────────────────────────────────────────

void UDomeLightStageCameraComponent::BuildDomeCameras()
{
    DomeCameras.Empty();
    switch (DomeConfig.Layout)
    {
    case EDomeCameraLayout::FibonacciSphere: BuildFibonacciSphere(); break;
    case EDomeCameraLayout::Rings:           BuildRingLayout();       break;
    case EDomeCameraLayout::Manual:          DomeCameras = DomeConfig.ManualCameras; break;
    }
    for (int32 i = 0; i < DomeCameras.Num(); i++) DomeCameras[i].CameraIndex = i;
}

void UDomeLightStageCameraComponent::BuildFibonacciSphere()
{
    const float GoldenAngle = PI * (3.0f - FMath::Sqrt(5.0f));
    int32 Total = DomeConfig.NumCameras;
    int32 SampleCount = DomeConfig.bUpperHemisphereOnly ? Total * 2 : Total;
    for (int32 i = 0; i < SampleCount; i++)
    {
        float y = 1.0f - (2.0f * i + 1.0f) / SampleCount;
        if (DomeConfig.bUpperHemisphereOnly && y < 0.0f) continue;
        float r = FMath::Sqrt(FMath::Max(0.0f, 1.0f - y * y));
        float theta = GoldenAngle * i;
        FVector Dir(r * FMath::Cos(theta), r * FMath::Sin(theta), y);
        Dir.Normalize();
        FDomeCameraView View;
        View.RelativePosition = Dir * DomeConfig.DomeRadius;
        View.LookAtRotation   = (-Dir).Rotation();
        View.FOV              = DomeConfig.CameraFOV;
        View.CameraName       = FString::Printf(TEXT("Cam_Fib_%03d"), DomeCameras.Num());
        DomeCameras.Add(View);
        if (DomeCameras.Num() >= Total) break;
    }
}

void UDomeLightStageCameraComponent::BuildRingLayout()
{
    int32 NumRings = FMath::Min(DomeConfig.CamerasPerRing.Num(), DomeConfig.RingElevations.Num());
    for (int32 Ring = 0; Ring < NumRings; Ring++)
    {
        int32 N = DomeConfig.CamerasPerRing[Ring];
        float ElevRad = FMath::DegreesToRadians(DomeConfig.RingElevations[Ring]);
        float cosE = FMath::Cos(ElevRad), sinE = FMath::Sin(ElevRad);
        for (int32 j = 0; j < N; j++)
        {
            float AzRad = FMath::DegreesToRadians(360.0f * j / N);
            FVector Dir(cosE * FMath::Cos(AzRad), cosE * FMath::Sin(AzRad), sinE);
            Dir.Normalize();
            FDomeCameraView View;
            View.RelativePosition = Dir * DomeConfig.DomeRadius;
            View.LookAtRotation   = (-Dir).Rotation();
            View.FOV              = DomeConfig.CameraFOV;
            View.CameraName       = FString::Printf(TEXT("Cam_R%d_%02d"), Ring, j);
            DomeCameras.Add(View);
        }
    }
}

// ─── Probe Pool ──────────────────────────────────────────────────────────────

void UDomeLightStageCameraComponent::RebuildProbePool()
{
    DestroyProbePool();
    int32 BatchSize = FMath::Clamp(DomeConfig.RenderBatchSize, 1, 16);
    for (int32 i = 0; i < BatchSize; i++)
    {
        USceneCaptureComponent2D* Cap = NewObject<USceneCaptureComponent2D>(
            this, *FString::Printf(TEXT("DomeProbe_%d"), i));
        Cap->RegisterComponent();
        Cap->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        Cap->bCaptureEveryFrame = false;
        Cap->bCaptureOnMovement = false;
        Cap->bAlwaysPersistRenderingState = true;

        UTextureRenderTarget2D* RGBRT = NewObject<UTextureRenderTarget2D>(this);
        RGBRT->InitCustomFormat(DomeConfig.Resolution.X, DomeConfig.Resolution.Y, PF_B8G8R8A8, false);
        RGBRT->UpdateResourceImmediate(true);
        Cap->TextureTarget = RGBRT;

        UTextureRenderTarget2D* AuxRT = NewObject<UTextureRenderTarget2D>(this);
        AuxRT->InitCustomFormat(DomeConfig.Resolution.X, DomeConfig.Resolution.Y, PF_A32B32G32R32F, false);
        AuxRT->UpdateResourceImmediate(true);

        ProbeCaptures.Add(Cap);
        ProbeRGBRTs.Add(RGBRT);
        ProbeAuxRTs.Add(AuxRT);
    }
}

void UDomeLightStageCameraComponent::DestroyProbePool()
{
    for (USceneCaptureComponent2D* Cap : ProbeCaptures)
        if (Cap) Cap->DestroyComponent();
    ProbeCaptures.Empty();
    ProbeRGBRTs.Empty();
    ProbeAuxRTs.Empty();
}

// ─── Warmup ──────────────────────────────────────────────────────────────────

void UDomeLightStageCameraComponent::CaptureFisheyeScene()
{
    if (ProbeCaptures.Num() == 0 || DomeCameras.Num() == 0) return;
    UpdateComponentToWorld();
    USceneCaptureComponent2D* WarmCap = ProbeCaptures[0];
    FVector WP = GetComponentLocation() + GetComponentRotation().RotateVector(DomeCameras[0].RelativePosition);
    WarmCap->SetWorldLocationAndRotation(WP, DomeCameras[0].LookAtRotation);
    WarmCap->UpdateComponentToWorld();

    static IConsoleVariable* VSMCacheCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Cache"));
    static IConsoleVariable* VSMInvCVar   = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.ForceInvalidate"));
    WarmCap->PostProcessSettings.bOverride_LumenSceneLightingQuality = true;
    WarmCap->PostProcessSettings.LumenSceneLightingQuality = 4.0f;
    for (int32 w = 0; w < DomeConfig.WarmUpFrames; w++)
    {
        if (VSMCacheCVar) VSMCacheCVar->Set(w == 0 ? 0 : 1, ECVF_SetByCode);
        if (VSMInvCVar)   VSMInvCVar->Set(w == 0 ? 1 : 0, ECVF_SetByCode);
        WarmCap->CaptureScene();
        FlushRenderingCommands();
    }
    if (VSMCacheCVar) VSMCacheCVar->Set(1, ECVF_SetByCode);
    if (VSMInvCVar)   VSMInvCVar->Set(0, ECVF_SetByCode);
}

// ─── AOV Helper ──────────────────────────────────────────────────────────────

void UDomeLightStageCameraComponent::RenderAndExportAOV(
    int32 ProbeIdx, UMaterialInterface* Material, UTextureRenderTarget2D* TargetRT,
    const FString& OutputPath, bool bUseHDRFormat, bool bDisableAA)
{
    if (!Material || !TargetRT || !ProbeCaptures.IsValidIndex(ProbeIdx)) return;
    USceneCaptureComponent2D* Cap = ProbeCaptures[ProbeIdx];

    UTextureRenderTarget2D* OrigRT = Cap->TextureTarget;
    FPostProcessSettings    OrigPPS = Cap->PostProcessSettings;
    ESceneCaptureSource     OrigSrc = Cap->CaptureSource;
    FEngineShowFlags        OrigFlags = Cap->ShowFlags;

    Cap->TextureTarget = TargetRT;
    Cap->PostProcessSettings.WeightedBlendables.Array.Empty();
    // Cap->PostProcessSettings.AddBlendable(Material, 1.0f);
    Cap->PostProcessSettings.AddBlendable(Material, 1.0f);
    Cap->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
    if (bDisableAA) { Cap->ShowFlags.SetTemporalAA(false); Cap->ShowFlags.SetAntiAliasing(false); }

    Cap->CaptureScene();
    FlushRenderingCommands();

    FImageWriteOptions Opts;
    Opts.Format = bUseHDRFormat ? EDesiredImageFormat::EXR : EDesiredImageFormat::PNG;
    Opts.bAsync = false;
    UImageWriteBlueprintLibrary::ExportToDisk(TargetRT, OutputPath, Opts);

    Cap->TextureTarget      = OrigRT;
    Cap->PostProcessSettings = OrigPPS;
    Cap->CaptureSource      = OrigSrc;
    Cap->ShowFlags          = OrigFlags;
}

// =============================================================================
// ── SEMANTIC：顶点色覆盖路径（主路径）────────────────────────────────────────
// =============================================================================

void UDomeLightStageCameraComponent::RebuildSemanticTextureCache()
{
    // 先清除旧的顶点色覆盖，避免残留
    for (auto& Pair : SemanticCache)
    {
        if (USkeletalMeshComponent* SkelComp = Pair.Key)
            SkelComp->ClearVertexColorOverride(0);
    }
    SemanticCache.Empty();
    BuildSemanticCacheForScene();
}

void UDomeLightStageCameraComponent::BuildSemanticCacheForScene()
{
    UWorld* World = GetWorld();
    if (!World) return;

    for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
    {
        TArray<USkeletalMeshComponent*> SkelComps;
        ActorIt->GetComponents<USkeletalMeshComponent>(SkelComps);
        for (USkeletalMeshComponent* SkelComp : SkelComps)
        {
            if (!SkelComp || !SkelComp->IsRegistered() || !SkelComp->GetSkeletalMeshAsset()) continue;
            if (!IsSemanticCandidateMesh(SkelComp)) continue;
            if (SemanticCache.Contains(SkelComp)) continue;

            TArray<FColor> Colors;
            if (!BuildSemanticColorsForMesh(SkelComp, Colors)) continue;

            UMaterialInstanceDynamic* MID = CreateSemanticVertexColorMID(SkelComp);
            if (!MID) continue;

            FSemanticMeshEntry Entry;
            Entry.SemanticColors = MoveTemp(Colors);
            Entry.SemanticMID    = MID;
            Entry.NumMaterials   = SkelComp->GetNumMaterials();
            SemanticCache.Add(SkelComp, MoveTemp(Entry));

            UE_LOG(LogTemp, Log, TEXT("[DomeLightStage] SemanticCache: %s (%d verts, %d mats)"),
                *SkelComp->GetName(),
                SemanticCache[SkelComp].SemanticColors.Num(),
                SemanticCache[SkelComp].NumMaterials);
        }
    }
    UE_LOG(LogTemp, Warning, TEXT("[DomeLightStage] SemanticCache total: %d SkelComps"), SemanticCache.Num());
}

// ─── BuildSemanticColorsForMesh ──────────────────────────────────────────────

bool UDomeLightStageCameraComponent::BuildSemanticColorsForMesh(
    USkeletalMeshComponent* SkelComp, TArray<FColor>& OutColors) const
{
    USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset();
    FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
    if (!RenderData || RenderData->LODRenderData.Num() == 0) return false;

    const FSkeletalMeshLODRenderData& LODData    = RenderData->LODRenderData[0];
    const FSkinWeightVertexBuffer*    SkinWeights = LODData.GetSkinWeightVertexBuffer();
    if (!SkinWeights) return false;

    const int32 NumVerts   = (int32)LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
    if (NumVerts == 0) return false;

    const FReferenceSkeleton& RefSkel  = SkelMesh->GetRefSkeleton();
    const int32               NumInf   = SkinWeights->GetMaxBoneInfluences();

    OutColors.SetNumUninitialized(NumVerts);
    for (int32 VertIdx = 0; VertIdx < NumVerts; ++VertIdx)
    {
        OutColors[VertIdx] = ComputeVertexSemanticColor(VertIdx, SkinWeights, RefSkel, NumInf)
                             .ToFColor(false);
    }
    return true;
}

// ─── ComputeVertexSemanticColor ───────────────────────────────────────────────

FLinearColor UDomeLightStageCameraComponent::ComputeVertexSemanticColor(
    int32 VertIdx,
    const FSkinWeightVertexBuffer* SkinWeights,
    const FReferenceSkeleton& RefSkel,
    int32 NumInfluences) const
{
    FLinearColor Blended    = FLinearColor::Black;
    float        TotalWeight = 0.0f;

    for (int32 Inf = 0; Inf < NumInfluences; ++Inf)
    {
        const uint16 BoneIdx   = SkinWeights->GetBoneIndex(VertIdx, Inf);
        const uint16 RawWeight = SkinWeights->GetBoneWeight(VertIdx, Inf);
        const float  Weight    = (float)RawWeight / 65535.0f;

        if (Weight <= KINDA_SMALL_NUMBER) break; // 权重已降序排列，后续均为 0

        if (!RefSkel.IsValidIndex(BoneIdx)) continue;

        const FLinearColor BoneColor =
            ResolveBoneSemanticColor(RefSkel.GetBoneName(BoneIdx).ToString());

        Blended     += BoneColor * Weight;
        TotalWeight += Weight;
    }

    if (TotalWeight > KINDA_SMALL_NUMBER)
    {
        Blended /= TotalWeight;
        Blended.A = 1.0f;
        Blended.R = FMath::Clamp(Blended.R, 0.f, 1.f);
        Blended.G = FMath::Clamp(Blended.G, 0.f, 1.f);
        Blended.B = FMath::Clamp(Blended.B, 0.f, 1.f);
        return Blended;
    }
    return GSemanticDefaultColor;
}

// ─── ResolveBoneSemanticColor ─────────────────────────────────────────────────
//
// 三轮匹配策略，按优先级依次执行：
//   1. Equals        完整骨骼名精确匹配（最高优先级，避免误匹配）
//   2. StartsWith    前缀匹配（覆盖 thumb_01_l / thumb_02_l 等多节骨骼）
//   3. Contains      包含匹配（覆盖 12IPV_ 插值骨骼，如 FACIAL_L_12IPV_ForeheadIn1）
//
// 注意：Contains 匹配时关键词越短越容易误匹配，已在表中选取区分度足够的关键词。

FLinearColor UDomeLightStageCameraComponent::ResolveBoneSemanticColor(
    const FString& BoneName) const
{
    // Pass 1：精确匹配
    for (const auto& Entry : GBoneSemanticMap)
    {
        if (BoneName.Equals(Entry.BoneKeyword, ESearchCase::IgnoreCase))
            return Entry.Color;
    }
    // Pass 2：前缀匹配
    for (const auto& Entry : GBoneSemanticMap)
    {
        if (BoneName.StartsWith(Entry.BoneKeyword, ESearchCase::IgnoreCase))
            return Entry.Color;
    }
    // Pass 3：包含匹配（兜底 12IPV_ 插值骨骼）
    for (const auto& Entry : GBoneSemanticMap)
    {
        if (BoneName.Contains(Entry.BoneKeyword, ESearchCase::IgnoreCase))
            return Entry.Color;
    }
    return GSemanticDefaultColor;
}

// ─── CreateSemanticVertexColorMID ────────────────────────────────────────────

UMaterialInstanceDynamic* UDomeLightStageCameraComponent::CreateSemanticVertexColorMID(
    USkeletalMeshComponent* SkelComp) const
{
    UMaterialInterface* BaseMat = nullptr;

    if (!SemanticVertexColorMaterialPath.IsEmpty())
        BaseMat = LoadObject<UMaterialInterface>(nullptr, *SemanticVertexColorMaterialPath);

#if WITH_EDITORONLY_DATA
    // 编辑器模式：自动创建 Unlit 材质，VertexColor → Emissive
    if (!BaseMat)
    {
        UMaterial* AutoMat = NewObject<UMaterial>(
            GetTransientPackage(),
            *FString::Printf(TEXT("M_SemanticVC_%s"), SkelComp ? *SkelComp->GetName() : TEXT("Mesh")),
            RF_Transient);
        if (!AutoMat) return nullptr;

        AutoMat->MaterialDomain = MD_Surface;
        AutoMat->BlendMode      = BLEND_Opaque;
        AutoMat->SetShadingModel(MSM_Unlit);
        AutoMat->bUsedWithSkeletalMesh = true;
        AutoMat->bUsedWithMorphTargets = true;

        // VertexColor 节点直连 Emissive（不依赖任何贴图或 UV）
        UMaterialExpressionVertexColor* VCExpr =
            NewObject<UMaterialExpressionVertexColor>(AutoMat);
        AutoMat->GetExpressionCollection().AddExpression(VCExpr);
        AutoMat->GetEditorOnlyData()->EmissiveColor.Expression = VCExpr;
        AutoMat->PostEditChange();

        BaseMat = AutoMat;
    }
#endif

    if (!BaseMat)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[DomeLightStage] No semantic base material. "
                 "Set SemanticVertexColorMaterialPath or use an editor build."));
        return nullptr;
    }

    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, GetTransientPackage());
    if (MID && MID->GetMaterial())
    {
        MID->GetMaterial()->bUsedWithMorphTargets = true;
    }
    return MID;
}

// ─── RenderSemanticWithVertexColor ────────────────────────────────────────────

void UDomeLightStageCameraComponent::RenderSemanticWithVertexColor(
    int32 ProbeIdx, UTextureRenderTarget2D* TargetRT, const FString& OutputPath)
{
    if (!TargetRT || !ProbeCaptures.IsValidIndex(ProbeIdx)) return;
    USceneCaptureComponent2D* Cap = ProbeCaptures[ProbeIdx];
    if (!Cap) return;

    // ── Step 1：对每个 SkelComp 写入顶点色 + 替换材质 ────────────────────────
    struct FRestoreEntry
    {
        USkeletalMeshComponent*      SkelComp;
        TArray<UMaterialInterface*>  OrigMats;
    };
    TArray<FRestoreEntry>         RestoreList;
    TArray<USkeletalMeshComponent*> ActiveComps;

    for (auto& Pair : SemanticCache)
    {
        USkeletalMeshComponent* SkelComp = Pair.Key;
        FSemanticMeshEntry&     Entry    = Pair.Value;

        if (!SkelComp || !SkelComp->IsRegistered() || !Entry.SemanticMID) continue;

        // 写入顶点色（Game Thread，只覆盖 LOD0）
        // 注意：UE5 的 SetVertexColorOverride_GameThread 接受 LODIndex + TArray<FColor>
        SkelComp->SetVertexColorOverride(0, Entry.SemanticColors);

        // 替换材质
        FRestoreEntry Restore;
        Restore.SkelComp = SkelComp;
        const int32 NumMats = SkelComp->GetNumMaterials();
        Restore.OrigMats.Reserve(NumMats);
        for (int32 mi = 0; mi < NumMats; ++mi)
        {
            Restore.OrigMats.Add(SkelComp->GetMaterial(mi));
            SkelComp->SetMaterial(mi, Entry.SemanticMID);
        }

        RestoreList.Add(MoveTemp(Restore));
        ActiveComps.Add(SkelComp);
    }

    if (ActiveComps.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[DomeLightStage] RenderSemanticWithVertexColor: no active comps."));
        return;
    }

    // ── Step 2：配置捕获（纯色，无光照，无 AA）────────────────────────────────
    UTextureRenderTarget2D*          OrigRT   = Cap->TextureTarget;
    FPostProcessSettings             OrigPPS  = Cap->PostProcessSettings;
    ESceneCaptureSource              OrigSrc  = Cap->CaptureSource;
    FEngineShowFlags                 OrigFlags = Cap->ShowFlags;
    ESceneCapturePrimitiveRenderMode OrigMode = Cap->PrimitiveRenderMode;

    Cap->TextureTarget  = TargetRT;
    Cap->CaptureSource  = ESceneCaptureSource::SCS_FinalColorLDR;
    Cap->PostProcessSettings.WeightedBlendables.Array.Empty();

    Cap->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Cap->ClearShowOnlyComponents();
    for (USkeletalMeshComponent* SkelComp : ActiveComps)
        Cap->ShowOnlyComponent(SkelComp);

    Cap->ShowFlags.SetVertexColors(true); // 必须开启，否则 VertexColor 节点输出白色
    Cap->ShowFlags.SetLighting(false);
    Cap->ShowFlags.SetSkyLighting(false);
    Cap->ShowFlags.SetAtmosphere(false);
    Cap->ShowFlags.SetFog(false);
    Cap->ShowFlags.SetVolumetricFog(false);
    Cap->ShowFlags.SetPostProcessing(false);
    Cap->ShowFlags.SetTemporalAA(false);
    Cap->ShowFlags.SetAntiAliasing(false);
    Cap->ShowFlags.SetMotionBlur(false);
    Cap->ShowFlags.SetBloom(false);
    Cap->ShowFlags.SetEyeAdaptation(false);
    Cap->ShowFlags.SetGrain(false);
    Cap->ShowFlags.SetVignette(false);

    if (UWorld* World = GetWorld())
        UKismetRenderingLibrary::ClearRenderTarget2D(World, TargetRT, FLinearColor::Black);

    // ── Step 3：捕获 ─────────────────────────────────────────────────────────
    Cap->CaptureScene();
    FlushRenderingCommands();

    FImageWriteOptions Opts;
    Opts.Format = EDesiredImageFormat::PNG;
    Opts.bAsync = false;
    UImageWriteBlueprintLibrary::ExportToDisk(TargetRT, OutputPath, Opts);

    // ── Step 4：还原材质 + 清除顶点色覆盖 ────────────────────────────────────
    Cap->ClearShowOnlyComponents();
    Cap->PrimitiveRenderMode = OrigMode;
    Cap->TextureTarget       = OrigRT;
    Cap->PostProcessSettings = OrigPPS;
    Cap->CaptureSource       = OrigSrc;
    Cap->ShowFlags           = OrigFlags;

    for (const FRestoreEntry& Restore : RestoreList)
    {
        if (!Restore.SkelComp) continue;
        // 清除顶点色覆盖，还原渲染状态
        Restore.SkelComp->ClearVertexColorOverride(0);
        for (int32 mi = 0; mi < Restore.OrigMats.Num(); ++mi)
            Restore.SkelComp->SetMaterial(mi, Restore.OrigMats[mi]);
    }
}

// ─── RenderAndExportSemantic（通用入口）──────────────────────────────────────

void UDomeLightStageCameraComponent::RenderAndExportSemantic(
    int32 ProbeIdx, UTextureRenderTarget2D* TargetRT, const FString& OutputPath)
{
    if (!TargetRT || !ProbeCaptures.IsValidIndex(ProbeIdx)) return;

    // 优先级 1：用户后处理材质（最简单，精度取决于材质）
    if (FisheyeSemanticPostProcessMaterial)
    {
        RenderAndExportAOV(ProbeIdx, FisheyeSemanticPostProcessMaterial, TargetRT, OutputPath, false, true);
        return;
    }

    // 优先级 2：顶点色覆盖路径（精细，主路径）
    if (SemanticCache.Num() > 0)
    {
        RenderSemanticWithVertexColor(ProbeIdx, TargetRT, OutputPath);
        return;
    }

    // 优先级 3：尝试延迟构建缓存（ConfigureDome 未调用时的兜底）
    BuildSemanticCacheForScene();
    if (SemanticCache.Num() > 0)
    {
        RenderSemanticWithVertexColor(ProbeIdx, TargetRT, OutputPath);
        return;
    }

    // 优先级 4：槽级材质替换（最粗粒度兜底）
    UE_LOG(LogTemp, Warning,
        TEXT("[DomeLightStage] Semantic: falling back to slot-level replacement."));
    EnsureSemanticRegionMaterials();
    if (SemanticRegionMaterials.Num() == 0) return;

    UWorld* World = GetWorld();
    USceneCaptureComponent2D* Cap = ProbeCaptures[ProbeIdx];
    if (!World || !Cap) return;

    struct FSlotRestore { TWeakObjectPtr<UMeshComponent> Mesh; TArray<UMaterialInterface*> Mats; };
    TArray<FSlotRestore>  RestoreList;
    TArray<UMeshComponent*> Meshes;

    for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
    {
        TArray<UMeshComponent*> Comps;
        ActorIt->GetComponents<UMeshComponent>(Comps);
        for (UMeshComponent* MC : Comps)
        {
            USkeletalMeshComponent* SC = Cast<USkeletalMeshComponent>(MC);
            if (!SC || !IsSemanticCandidateMesh(SC)) continue;
            FSlotRestore R; R.Mesh = MC;
            for (int32 mi = 0; mi < MC->GetNumMaterials(); ++mi)
            {
                R.Mats.Add(MC->GetMaterial(mi));
                FName Region = GetSemanticRegionForMeshSlot(MC, mi);
                UMaterialInterface* const* Mat = SemanticRegionMaterials.Find(Region);
                MC->SetMaterial(mi, Mat ? *Mat : SemanticRegionMaterials.FindRef(TEXT("Body")));
            }
            RestoreList.Add(MoveTemp(R));
            Meshes.Add(MC);
        }
    }

    auto OrigRT   = Cap->TextureTarget; auto OrigPPS = Cap->PostProcessSettings;
    auto OrigSrc  = Cap->CaptureSource; auto OrigFlags = Cap->ShowFlags;
    auto OrigMode = Cap->PrimitiveRenderMode;

    Cap->TextureTarget = TargetRT;
    Cap->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    Cap->PostProcessSettings.WeightedBlendables.Array.Empty();
    Cap->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Cap->ClearShowOnlyComponents();
    for (UMeshComponent* MC : Meshes) Cap->ShowOnlyComponent(MC);
    Cap->ShowFlags.SetLighting(false); Cap->ShowFlags.SetPostProcessing(false);
    Cap->ShowFlags.SetTemporalAA(false); Cap->ShowFlags.SetAntiAliasing(false);
    Cap->ShowFlags.SetMotionBlur(false); Cap->ShowFlags.SetBloom(false);
    Cap->ShowFlags.SetEyeAdaptation(false); Cap->ShowFlags.SetGrain(false);

    Cap->CaptureScene(); FlushRenderingCommands();

    FImageWriteOptions Opts; Opts.Format = EDesiredImageFormat::PNG; Opts.bAsync = false;
    UImageWriteBlueprintLibrary::ExportToDisk(TargetRT, OutputPath, Opts);

    Cap->ClearShowOnlyComponents(); Cap->PrimitiveRenderMode = OrigMode;
    Cap->TextureTarget = OrigRT; Cap->PostProcessSettings = OrigPPS;
    Cap->CaptureSource = OrigSrc; Cap->ShowFlags = OrigFlags;
    for (auto& R : RestoreList)
        if (UMeshComponent* MC = R.Mesh.Get())
            for (int32 mi = 0; mi < R.Mats.Num(); ++mi)
                MC->SetMaterial(mi, R.Mats[mi]);
}

// =============================================================================
// ── 辅助函数 ──────────────────────────────────────────────────────────────────
// =============================================================================

bool UDomeLightStageCameraComponent::IsSemanticCandidateMesh(
    USkeletalMeshComponent* SkelComp) const
{
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset()) return false;

    // MetaHuman Face mesh：含 FACIAL_ 前缀骨骼
    const FReferenceSkeleton& RefSkel = SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton();
    for (int32 BoneIdx = 0; BoneIdx < RefSkel.GetNum(); ++BoneIdx)
    {
        const FString BoneName = RefSkel.GetBoneName(BoneIdx).ToString();
        if (BoneName.StartsWith(TEXT("FACIAL_"), ESearchCase::IgnoreCase))
            return true;
    }
    // MetaHuman Body mesh：含标准 Mannequin 骨骼
    auto HasBone = [&](const TCHAR* Name) {
        return SkelComp->GetBoneIndex(FName(Name)) != INDEX_NONE;
    };
    return HasBone(TEXT("spine_01")) || HasBone(TEXT("pelvis")) || HasBone(TEXT("head"));
}

bool UDomeLightStageCameraComponent::HasBoneName(
    UMeshComponent* MeshComp, const FName& BoneName) const
{
    USkeletalMeshComponent* SC = Cast<USkeletalMeshComponent>(MeshComp);
    return SC && SC->GetBoneIndex(BoneName) != INDEX_NONE;
}

bool UDomeLightStageCameraComponent::HasBoneNameContaining(
    UMeshComponent* MeshComp, const FString& BoneNamePart) const
{
    USkeletalMeshComponent* SC = Cast<USkeletalMeshComponent>(MeshComp);
    if (!SC || !SC->GetSkeletalMeshAsset()) return false;
    const FReferenceSkeleton& Ref = SC->GetSkeletalMeshAsset()->GetRefSkeleton();
    for (int32 i = 0; i < Ref.GetNum(); ++i)
        if (Ref.GetBoneName(i).ToString().Contains(BoneNamePart, ESearchCase::IgnoreCase))
            return true;
    return false;
}

// ─── 槽级材质兜底 ─────────────────────────────────────────────────────────────

UMaterialInterface* UDomeLightStageCameraComponent::CreateSemanticColorMaterial(
    const FName& RegionName, const FLinearColor& Color)
{
#if WITH_EDITORONLY_DATA
    UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(),
        *FString::Printf(TEXT("M_Semantic_%s"), *RegionName.ToString()), RF_Transient);
    if (!Mat) return nullptr;
    Mat->MaterialDomain = MD_Surface;
    Mat->BlendMode      = BLEND_Opaque;
    Mat->SetShadingModel(MSM_Unlit);
    Mat->bUsedWithSkeletalMesh = true;
    Mat->bUsedWithMorphTargets = true;
    UMaterialExpressionConstant3Vector* CE = NewObject<UMaterialExpressionConstant3Vector>(Mat);
    CE->Constant = Color;
    Mat->GetExpressionCollection().AddExpression(CE);
    Mat->GetEditorOnlyData()->EmissiveColor.Expression = CE;
    Mat->GetEditorOnlyData()->BaseColor.Expression     = CE;
    Mat->PostEditChange();
    return Mat;
#else
    return nullptr;
#endif
}

void UDomeLightStageCameraComponent::EnsureSemanticRegionMaterials()
{
    if (SemanticRegionMaterials.Num() > 0) return;
    struct RC { FName R; FLinearColor C; };
    const RC Table[] = {
        {TEXT("Face"),  {1,0,0,1}}, {TEXT("Eyes"),  {0,1,1,1}}, {TEXT("Teeth"), {1,1,1,1}},
        {TEXT("Hair"),  {1,0,1,1}}, {TEXT("Torso"), {0,1,0,1}}, {TEXT("Arms"),  {1,1,0,1}},
        {TEXT("Hands"), {1,.5,0,1}},{TEXT("Legs"),  {0,0,1,1}}, {TEXT("Feet"),  {.5,0,1,1}},
        {TEXT("Body"),  {0,.75,.25,1}},
    };
    for (const RC& E : Table)
        if (UMaterialInterface* M = CreateSemanticColorMaterial(E.R, E.C))
            SemanticRegionMaterials.Add(E.R, M);
}

FName UDomeLightStageCameraComponent::GetSemanticRegionForMeshSlot(
    UMeshComponent* MeshComp, int32 MaterialIndex) const
{
    if (!MeshComp) return TEXT("Body");

    // Face SkelMesh：含 FACIAL_C_FacialRoot
    if (HasBoneName(MeshComp, TEXT("FACIAL_C_FacialRoot")))
    {
        const TArray<FName> Slots = MeshComp->GetMaterialSlotNames();
        FString S = Slots.IsValidIndex(MaterialIndex) ? Slots[MaterialIndex].ToString().ToLower() : TEXT("");
        if (S.Contains(TEXT("eye")) || S.Contains(TEXT("cornea")))  return TEXT("Eyes");
        if (S.Contains(TEXT("teeth")) || S.Contains(TEXT("tongue"))) return TEXT("Teeth");
        if (S.Contains(TEXT("hair")) || S.Contains(TEXT("lash")))    return TEXT("Hair");
        return TEXT("Face");
    }
    // Body SkelMesh
    if (HasBoneName(MeshComp, TEXT("spine_01")) || HasBoneName(MeshComp, TEXT("pelvis")))
    {
        const TArray<FName> Slots = MeshComp->GetMaterialSlotNames();
        FString S = Slots.IsValidIndex(MaterialIndex) ? Slots[MaterialIndex].ToString().ToLower() : TEXT("");
        if (S.Contains(TEXT("hand")) || S.Contains(TEXT("finger")))  return TEXT("Hands");
        if (S.Contains(TEXT("foot")) || S.Contains(TEXT("toe")))     return TEXT("Feet");
        if (S.Contains(TEXT("leg"))  || S.Contains(TEXT("thigh")))   return TEXT("Legs");
        if (S.Contains(TEXT("arm"))  || S.Contains(TEXT("elbow")))   return TEXT("Arms");
        return TEXT("Torso");
    }
    return TEXT("Body");
}

// ─── 内参 / 外参 JSON ─────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> UDomeLightStageCameraComponent::BuildIntrinsicJson(
    float FOVDeg, FIntPoint Res) const
{
    float HalfFOVRad = FMath::DegreesToRadians(FOVDeg * 0.5f);
    float fx = (Res.X * 0.5f) / FMath::Tan(HalfFOVRad);
    float cx = Res.X * 0.5f, cy = Res.Y * 0.5f;
    TArray<TSharedPtr<FJsonValue>> Rows;
    auto Row = [&](float a, float b, float c) {
        TArray<TSharedPtr<FJsonValue>> R;
        R.Add(MakeShareable(new FJsonValueNumber(a)));
        R.Add(MakeShareable(new FJsonValueNumber(b)));
        R.Add(MakeShareable(new FJsonValueNumber(c)));
        return MakeShareable(new FJsonValueArray(R));
    };
    Rows.Add(Row(fx,0,cx)); Rows.Add(Row(0,fx,cy)); Rows.Add(Row(0,0,1));
    TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
    Obj->SetArrayField(TEXT("matrix"), Rows);
    Obj->SetNumberField(TEXT("fx"), fx); Obj->SetNumberField(TEXT("fy"), fx);
    Obj->SetNumberField(TEXT("cx"), cx); Obj->SetNumberField(TEXT("cy"), cy);
    Obj->SetNumberField(TEXT("fov_deg"), FOVDeg);
    return Obj;
}

TSharedPtr<FJsonObject> UDomeLightStageCameraComponent::BuildExtrinsicJson(
    const FVector& WorldPos, const FQuat& WorldRot) const
{
    FMatrix M = FQuatRotationTranslationMatrix(WorldRot, WorldPos);
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (int32 r = 0; r < 4; r++) {
        TArray<TSharedPtr<FJsonValue>> R;
        for (int32 c = 0; c < 4; c++)
            R.Add(MakeShareable(new FJsonValueNumber(M.M[r][c])));
        Rows.Add(MakeShareable(new FJsonValueArray(R)));
    }
    TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject);
    Obj->SetArrayField(TEXT("cam_to_world"), Rows);
    TArray<TSharedPtr<FJsonValue>> P, Q;
    P.Add(MakeShareable(new FJsonValueNumber(WorldPos.X)));
    P.Add(MakeShareable(new FJsonValueNumber(WorldPos.Y)));
    P.Add(MakeShareable(new FJsonValueNumber(WorldPos.Z)));
    Q.Add(MakeShareable(new FJsonValueNumber(WorldRot.X)));
    Q.Add(MakeShareable(new FJsonValueNumber(WorldRot.Y)));
    Q.Add(MakeShareable(new FJsonValueNumber(WorldRot.Z)));
    Q.Add(MakeShareable(new FJsonValueNumber(WorldRot.W)));
    Obj->SetArrayField(TEXT("world_pos"), P);
    Obj->SetArrayField(TEXT("world_rot_quat"), Q);
    return Obj;
}

// ─── SaveAllData ──────────────────────────────────────────────────────────────

bool UDomeLightStageCameraComponent::SaveAllData(const FString& BasePath, int32 FrameNumber)
{
    if (DomeCameras.Num() == 0 || ProbeCaptures.Num() == 0) return false;

    int32 FrameNum = (FrameNumber >= 0) ? FrameNumber : FrameCounter;
    FString FrameFolder = FString::Printf(TEXT("%s/Frame_%04d"), *BasePath, FrameNum);
    IFileManager::Get().MakeDirectory(*FrameFolder, true);

    const int32   BatchSize = ProbeCaptures.Num();
    const FVector CenterWS  = GetComponentLocation();
    const FRotator BaseRot  = GetComponentRotation();

    bool bDoRGB      = (DomeConfig.EnabledPasses & (uint8)EDomeRenderPass::RGB)      != 0;
    bool bDoDepth    = (DomeConfig.EnabledPasses & (uint8)EDomeRenderPass::Depth)    != 0 && FisheyeDepthPostProcessMaterial;
    bool bDoNormal   = (DomeConfig.EnabledPasses & (uint8)EDomeRenderPass::Normal)   != 0 && FisheyeNormalPostProcessMaterial;
    bool bDoPosition = (DomeConfig.EnabledPasses & (uint8)EDomeRenderPass::Position) != 0 && FisheyePositionPostProcessMaterial;
    bool bDoSemantic = (DomeConfig.EnabledPasses & (uint8)EDomeRenderPass::Semantic) != 0;

    TArray<TSharedPtr<FJsonValue>> CamerasJson;
    static IConsoleVariable* VSMCacheCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Cache"));
    static IConsoleVariable* VSMInvCVar   = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.ForceInvalidate"));
    const int32 PerCamWarmup = FMath::Max(3, DomeConfig.WarmUpFrames);

    for (int32 i = 0; i < DomeCameras.Num(); i += BatchSize)
    {
        int32 CurBatch = FMath::Min(BatchSize, DomeCameras.Num() - i);

        for (int32 j = 0; j < CurBatch; j++)
        {
            const FDomeCameraView& View = DomeCameras[i + j];
            FVector  WP = CenterWS + BaseRot.RotateVector(View.RelativePosition);
            FRotator WR = View.LookAtRotation + BaseRot;
            ProbeCaptures[j]->FOVAngle = View.FOV;
            ProbeCaptures[j]->SetWorldLocationAndRotation(WP, WR);
            ProbeCaptures[j]->UpdateComponentToWorld();
        }

        if (bDoRGB)
        {
            for (int32 j = 0; j < CurBatch; j++)
            {
                ProbeCaptures[j]->TextureTarget = ProbeRGBRTs[j];
                ProbeCaptures[j]->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
                ProbeCaptures[j]->ShowFlags.SetTemporalAA(true);
                ProbeCaptures[j]->ShowFlags.SetAntiAliasing(true);
                ProbeCaptures[j]->ShowFlags.SetMotionBlur(false);
                if (VSMCacheCVar) VSMCacheCVar->Set(0, ECVF_SetByCode);
                if (VSMInvCVar)   VSMInvCVar->Set(1, ECVF_SetByCode);
                ProbeCaptures[j]->CaptureScene(); FlushRenderingCommands();
                if (VSMCacheCVar) VSMCacheCVar->Set(1, ECVF_SetByCode);
                if (VSMInvCVar)   VSMInvCVar->Set(0, ECVF_SetByCode);
                for (int32 w = 1; w < PerCamWarmup; w++)
                    { ProbeCaptures[j]->CaptureScene(); FlushRenderingCommands(); }
            }
        }

        for (int32 j = 0; j < CurBatch; j++)
        {
            int32 CamIdx = i + j;
            const FDomeCameraView& View = DomeCameras[CamIdx];
            FString Prefix = FString::Printf(TEXT("%s/%s"), *FrameFolder, *View.CameraName);
            FVector WP = ProbeCaptures[j]->GetComponentLocation();
            FQuat   WQ = ProbeCaptures[j]->GetComponentQuat();

            TSharedPtr<FJsonObject> CamObj = MakeShareable(new FJsonObject);
            CamObj->SetNumberField(TEXT("camera_index"), CamIdx);
            CamObj->SetStringField(TEXT("camera_name"),  View.CameraName);
            CamObj->SetNumberField(TEXT("fov"),          View.FOV);
            CamObj->SetObjectField(TEXT("intrinsic"),    BuildIntrinsicJson(View.FOV, DomeConfig.Resolution));
            CamObj->SetObjectField(TEXT("extrinsic"),    BuildExtrinsicJson(WP, WQ));
            TArray<TSharedPtr<FJsonValue>> ResArr;
            ResArr.Add(MakeShareable(new FJsonValueNumber(DomeConfig.Resolution.X)));
            ResArr.Add(MakeShareable(new FJsonValueNumber(DomeConfig.Resolution.Y)));
            CamObj->SetArrayField(TEXT("resolution"), ResArr);

            if (bDoRGB) {
                FString Path = Prefix + TEXT("_RGB.png");
                FImageWriteOptions O; O.Format = EDesiredImageFormat::PNG; O.bAsync = false;
                UImageWriteBlueprintLibrary::ExportToDisk(ProbeRGBRTs[j], Path, O);
                CamObj->SetStringField(TEXT("rgb_filename"), View.CameraName + TEXT("_RGB.png"));
            }
            if (bDoDepth) {
                FString Path = Prefix + TEXT("_Depth.exr");
                RenderAndExportAOV(j, FisheyeDepthPostProcessMaterial, ProbeAuxRTs[j], Path, true);
                CamObj->SetStringField(TEXT("depth_filename"), View.CameraName + TEXT("_Depth.exr"));
            }
            if (bDoNormal) {
                FString Path = Prefix + TEXT("_Normal.exr");
                RenderAndExportAOV(j, FisheyeNormalPostProcessMaterial, ProbeAuxRTs[j], Path, true);
                CamObj->SetStringField(TEXT("normal_filename"), View.CameraName + TEXT("_Normal.exr"));
            }
            if (bDoPosition) {
                FString Path = Prefix + TEXT("_Position.exr");
                RenderAndExportAOV(j, FisheyePositionPostProcessMaterial, ProbeAuxRTs[j], Path, true);
                CamObj->SetStringField(TEXT("position_filename"), View.CameraName + TEXT("_Position.exr"));
            }
            if (bDoSemantic) {
                UTextureRenderTarget2D* SemRT = NewObject<UTextureRenderTarget2D>(this);
                SemRT->InitCustomFormat(DomeConfig.Resolution.X, DomeConfig.Resolution.Y, PF_B8G8R8A8, false);
                SemRT->UpdateResourceImmediate(true);
                FString Path = Prefix + TEXT("_Semantic.png");
                RenderAndExportSemantic(j, SemRT, Path);
                CamObj->SetStringField(TEXT("semantic_filename"), View.CameraName + TEXT("_Semantic.png"));
                SemRT->ConditionalBeginDestroy();
            }
            CamerasJson.Add(MakeShareable(new FJsonValueObject(CamObj)));
        }
    }

    TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject);
    TSharedPtr<FJsonObject> DomeMeta = MakeShareable(new FJsonObject);
    DomeMeta->SetNumberField(TEXT("radius"),      DomeConfig.DomeRadius);
    DomeMeta->SetNumberField(TEXT("num_cameras"), DomeCameras.Num());
    DomeMeta->SetNumberField(TEXT("fov"),         DomeConfig.CameraFOV);
    FString LayoutStr;
    switch (DomeConfig.Layout) {
        case EDomeCameraLayout::FibonacciSphere: LayoutStr = TEXT("fibonacci_sphere"); break;
        case EDomeCameraLayout::Rings:           LayoutStr = TEXT("rings");            break;
        case EDomeCameraLayout::Manual:          LayoutStr = TEXT("manual");           break;
    }
    DomeMeta->SetStringField(TEXT("layout"), LayoutStr);
    FVector CL = GetComponentLocation();
    TArray<TSharedPtr<FJsonValue>> CP;
    CP.Add(MakeShareable(new FJsonValueNumber(CL.X)));
    CP.Add(MakeShareable(new FJsonValueNumber(CL.Y)));
    CP.Add(MakeShareable(new FJsonValueNumber(CL.Z)));
    DomeMeta->SetArrayField(TEXT("center_world_pos"), CP);
    Root->SetObjectField(TEXT("dome_config"), DomeMeta);
    Root->SetArrayField(TEXT("cameras"), CamerasJson);

    FString JsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
    if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
        FFileHelper::SaveStringToFile(JsonStr, *(FrameFolder / TEXT("dome_cameras_metadata.json")));

    ++FrameCounter;
    UE_LOG(LogTemp, Warning, TEXT("[DomeLightStage] SaveAllData done: %d cameras -> %s"),
        DomeCameras.Num(), *FrameFolder);
    return true;
}