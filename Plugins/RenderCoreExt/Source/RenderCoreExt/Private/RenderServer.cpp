#include "RenderServer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "DataExporter.h"
#include "HAL/UnrealMemory.h"
#include "Misc/ByteSwap.h"
#include "FisheyeCameraComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/RectLight.h"
#include "Components/RectLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/PostProcessVolume.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneSequencePlayer.h"
#include "MultiViewFisheyeCameraComponent.h"
#include "MultiViewERPCameraComponent.h"
#include "DomeLightStageCameraComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
static bool IsMetaHumanFaceComponent(USkeletalMeshComponent* SkelComp)
{
    if (!SkelComp || !SkelComp->GetSkeletalMeshAsset()) return false;
    const FString CompName  = SkelComp->GetName();
    const FString AssetName = SkelComp->GetSkeletalMeshAsset()->GetName();
    return CompName.Contains(TEXT("Face"), ESearchCase::IgnoreCase)
        || AssetName.Contains(TEXT("face"), ESearchCase::IgnoreCase);
}

static int32 InjectRigLogicCurvesOnAnimInstance(
    UAnimInstance* AnimInst, const TMap<FName, float>& Curves, float* OutJawOpen = nullptr)
{
    if (!AnimInst) return 0;

    int32 CtrlCount = 0;
    for (const TPair<FName, float>& Curve : Curves)
    {
        const FString CurveName = Curve.Key.ToString();
        if (!CurveName.StartsWith(TEXT("CTRL_"))) continue;

        AnimInst->OverrideCurveValue(Curve.Key, Curve.Value);
        ++CtrlCount;

        if (OutJawOpen && CurveName.Contains(TEXT("jawOpen"), ESearchCase::IgnoreCase))
        {
            *OutJawOpen = Curve.Value;
        }
    }
    return CtrlCount;
}

static void RelaxRigLogicLodThreshold(UAnimInstance* AnimInst)
{
    if (!AnimInst) return;

    for (TFieldIterator<FProperty> It(AnimInst->GetClass()); It; ++It)
    {
        FProperty* Prop = *It;
        const FString PropName = Prop->GetName();
        if (!PropName.Contains(TEXT("RigLogic"), ESearchCase::IgnoreCase) ||
            !PropName.Contains(TEXT("LOD"), ESearchCase::IgnoreCase))
        {
            continue;
        }

        if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
        {
            IntProp->SetPropertyValue_InContainer(AnimInst, 255);
        }
        else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
        {
            FloatProp->SetPropertyValue_InContainer(AnimInst, 255.f);
        }
    }
}

static void CollectFaceAnimInstances(USkeletalMeshComponent* SkelComp, TArray<UAnimInstance*>& OutInstances)
{
    OutInstances.Reset();
    auto AddUnique = [&OutInstances](UAnimInstance* Inst)
    {
        if (Inst && !OutInstances.Contains(Inst))
        {
            OutInstances.Add(Inst);
        }
    };

    // 主 AnimBP 优先（Face_AnimBP_C），其次 PostProcess（RigLogic）
    AddUnique(SkelComp->GetAnimInstance());
    AddUnique(SkelComp->GetPostProcessInstance());

    const TArray<UAnimInstance*> SeedInstances = OutInstances;
    static const FName LinkedGroups[] = {
        FName(TEXT("Face")),
        FName(TEXT("FaceMesh")),
        FName(TEXT("DefaultSlot")),
    };
    for (UAnimInstance* Seed : SeedInstances)
    {
        for (const FName& Group : LinkedGroups)
        {
            TArray<UAnimInstance*> LinkedLayers;
            Seed->GetLinkedAnimLayerInstancesByGroup(Group, LinkedLayers);
            for (UAnimInstance* LinkedInst : LinkedLayers)
            {
                AddUnique(LinkedInst);
            }
        }
    }
}

static void PrepareFaceForRigLogicCapture(USkeletalMeshComponent* SkelComp)
{
    if (!SkelComp) return;

    SkelComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    SkelComp->bEnableUpdateRateOptimizations = false;
    SkelComp->SetDisablePostProcessBlueprint(false);
    SkelComp->bPauseAnims = false;
    SkelComp->SetForcedLOD(1);   // 1 = 强制 LOD0，避免 Rig Logic LOD Threshold 跳过节点
}

static void DetachFaceFromSequencerControlRig(USkeletalMeshComponent* SkelComp)
{
    if (!SkelComp) return;

    UAnimInstance* CurrentInst = SkelComp->GetAnimInstance();
    const FString CurrentClassName = CurrentInst ? CurrentInst->GetClass()->GetName() : TEXT("None");
    const bool bLooksLikeSequencerLayer =
        CurrentClassName.Contains(TEXT("ControlRigLayerInstance"), ESearchCase::IgnoreCase);

    if (SkelComp->GetAnimationMode() != EAnimationMode::AnimationBlueprint || bLooksLikeSequencerLayer)
    {
        UClass* ConfiguredAnimClass = SkelComp->GetAnimClass();
        SkelComp->SetAnimationMode(EAnimationMode::AnimationBlueprint);
        if (ConfiguredAnimClass)
        {
            SkelComp->SetAnimInstanceClass(ConfiguredAnimClass);
        }
        SkelComp->InitAnim(true);

        UAnimInstance* NewInst = SkelComp->GetAnimInstance();
        UE_LOG(LogTemp, Warning, TEXT("[LevelSequence][ARKit] Face '%s' detached from Sequencer layer (%s -> %s)"),
            *SkelComp->GetName(),
            *CurrentClassName,
            NewInst ? *NewInst->GetClass()->GetName() : TEXT("None"));
    }
}

static void PrepareMetaHumanForExternalFaceOverride(AActor* TargetActor)
{
    if (!TargetActor) return;

    TArray<USkeletalMeshComponent*> SkelComps;
    TargetActor->GetComponents<USkeletalMeshComponent>(SkelComps, true);
    for (USkeletalMeshComponent* SkelComp : SkelComps)
    {
        if (!IsMetaHumanFaceComponent(SkelComp)) continue;
        DetachFaceFromSequencerControlRig(SkelComp);
        PrepareFaceForRigLogicCapture(SkelComp);
    }
}

static void InjectCurvesOnFaceAnimTargets(
    const TArray<UAnimInstance*>& AnimTargets, const TMap<FName, float>& Curves, float* OutJawOpen)
{
    // 先写主 AnimBP（Face_AnimBP_C），再写 PostProcess（RigLogic 所在图）
    for (UAnimInstance* TargetInst : AnimTargets)
    {
        if (!TargetInst) continue;
        RelaxRigLogicLodThreshold(TargetInst);
        InjectRigLogicCurvesOnAnimInstance(TargetInst, Curves, OutJawOpen);
    }
}

static void ForceFaceRigLogicEvaluation(
    USkeletalMeshComponent* SkelComp,
    const TArray<UAnimInstance*>& AnimTargets,
    const TMap<FName, float>& Curves)
{
    if (!SkelComp) return;

    constexpr float ForceDelta = 1.0f / 60.0f;

    // ① 注入 → ② Tick（RigLogic 读曲线）→ ③ 再注入（防止 Face_AnimBP_C 当帧覆盖）
    InjectCurvesOnFaceAnimTargets(AnimTargets, Curves, nullptr);
    for (int32 i = 0; i < 3; ++i)
    {
        SkelComp->TickAnimation(ForceDelta, false);
    }
    InjectCurvesOnFaceAnimTargets(AnimTargets, Curves, nullptr);

    SkelComp->RefreshBoneTransforms();
    SkelComp->FinalizeBoneTransform();
    SkelComp->MarkRenderTransformDirty();
    SkelComp->MarkRenderDynamicDataDirty();
    SkelComp->MarkRenderStateDirty();
}

void ParseCurveObject(const TSharedPtr<FJsonObject>& JsonObject, TMap<FName, float>& OutCurves)
{
    OutCurves.Empty();
    if (!JsonObject.IsValid()) return;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : JsonObject->Values)
    {
        if (!Pair.Value.IsValid()) continue;
        OutCurves.Add(FName(*Pair.Key), static_cast<float>(Pair.Value->AsNumber()));
    }
}

static FString NormalizeARKitCoeffName(FString Name)
{
    Name = Name.ToLower();
    Name.ReplaceInline(TEXT("_"), TEXT(""));
    Name.ReplaceInline(TEXT("-"), TEXT(""));
    Name.ReplaceInline(TEXT(" "), TEXT(""));

    if (Name.EndsWith(TEXT("left")))
    {
        Name = Name.LeftChop(4) + TEXT("l");
    }
    else if (Name.EndsWith(TEXT("right")))
    {
        Name = Name.LeftChop(5) + TEXT("r");
    }

    return Name;
}

static bool FindARKitCoeff(const TMap<FString, float>& Coeffs, const TCHAR* Name, float& OutValue)
{
    const FString Target(Name);
    if (const float* Direct = Coeffs.Find(Target))
    {
        OutValue = *Direct;
        return true;
    }

    const FString NormalizedTarget = NormalizeARKitCoeffName(Target);
    for (const TPair<FString, float>& Pair : Coeffs)
    {
        if (Pair.Key.Equals(Target, ESearchCase::IgnoreCase) ||
            NormalizeARKitCoeffName(Pair.Key).Equals(NormalizedTarget, ESearchCase::CaseSensitive))
        {
            OutValue = Pair.Value;
            return true;
        }
    }
    return false;
}

static void AddCtrlCurve(TMap<FName, float>& OutCurves, const TCHAR* CtrlName, float Value, float Scale = 1.0f)
{
    OutCurves.Add(FName(CtrlName), FMath::Clamp(Value * Scale, 0.0f, 1.0f));
}

static void MapARKitCoeff(
    const TMap<FString, float>& Coeffs,
    const TCHAR* ARKitName,
    TMap<FName, float>& OutCurves,
    const TCHAR* CtrlName,
    float Scale = 1.0f)
{
    float Value = 0.0f;
    if (FindARKitCoeff(Coeffs, ARKitName, Value))
    {
        AddCtrlCurve(OutCurves, CtrlName, Value, Scale);
    }
}

static TMap<FName, float> ConvertARKitToMetaHumanCtrlCurves(const TMap<FName, float>& ARKitCurves)
{
    TMap<FString, float> Coeffs;
    for (const TPair<FName, float>& Curve : ARKitCurves)
    {
        Coeffs.Add(Curve.Key.ToString(), Curve.Value);
    }

    TMap<FName, float> CtrlCurves;
    CtrlCurves.Add(FName(TEXT("CTRL_rigLogic_OffOn")), 1.0f);

    MapARKitCoeff(Coeffs, TEXT("eyeBlink_L"), CtrlCurves, TEXT("CTRL_expressions_eyeBlinkL"));
    MapARKitCoeff(Coeffs, TEXT("eyeBlink_R"), CtrlCurves, TEXT("CTRL_expressions_eyeBlinkR"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookDown_L"), CtrlCurves, TEXT("CTRL_expressions_eyeLookDownL"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookDown_R"), CtrlCurves, TEXT("CTRL_expressions_eyeLookDownR"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookIn_L"), CtrlCurves, TEXT("CTRL_expressions_eyeLookRightL"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookIn_R"), CtrlCurves, TEXT("CTRL_expressions_eyeLookLeftR"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookOut_L"), CtrlCurves, TEXT("CTRL_expressions_eyeLookLeftL"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookOut_R"), CtrlCurves, TEXT("CTRL_expressions_eyeLookRightR"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookUp_L"), CtrlCurves, TEXT("CTRL_expressions_eyeLookUpL"));
    MapARKitCoeff(Coeffs, TEXT("eyeLookUp_R"), CtrlCurves, TEXT("CTRL_expressions_eyeLookUpR"));
    MapARKitCoeff(Coeffs, TEXT("eyeSquint_L"), CtrlCurves, TEXT("CTRL_expressions_eyeSquintL"));
    MapARKitCoeff(Coeffs, TEXT("eyeSquint_R"), CtrlCurves, TEXT("CTRL_expressions_eyeSquintR"));
    MapARKitCoeff(Coeffs, TEXT("eyeWide_L"), CtrlCurves, TEXT("CTRL_expressions_eyeWideL"));
    MapARKitCoeff(Coeffs, TEXT("eyeWide_R"), CtrlCurves, TEXT("CTRL_expressions_eyeWideR"));

    MapARKitCoeff(Coeffs, TEXT("jawForward"), CtrlCurves, TEXT("CTRL_expressions_jawFwd"));
    MapARKitCoeff(Coeffs, TEXT("jawLeft"), CtrlCurves, TEXT("CTRL_expressions_jawLeft"));
    MapARKitCoeff(Coeffs, TEXT("jawRight"), CtrlCurves, TEXT("CTRL_expressions_jawRight"));
    MapARKitCoeff(Coeffs, TEXT("jawOpen"), CtrlCurves, TEXT("CTRL_expressions_jawOpen"));
    MapARKitCoeff(Coeffs, TEXT("mouthClose"), CtrlCurves, TEXT("CTRL_expressions_mouthClose"));
    MapARKitCoeff(Coeffs, TEXT("mouthFunnel"), CtrlCurves, TEXT("CTRL_expressions_mouthFunnel"));
    MapARKitCoeff(Coeffs, TEXT("mouthPucker"), CtrlCurves, TEXT("CTRL_expressions_mouthPucker"));
    MapARKitCoeff(Coeffs, TEXT("mouthLeft"), CtrlCurves, TEXT("CTRL_expressions_mouthLeft"));
    MapARKitCoeff(Coeffs, TEXT("mouthRight"), CtrlCurves, TEXT("CTRL_expressions_mouthRight"));
    MapARKitCoeff(Coeffs, TEXT("mouthSmile_L"), CtrlCurves, TEXT("CTRL_expressions_mouthCornerPullL"));
    MapARKitCoeff(Coeffs, TEXT("mouthSmile_R"), CtrlCurves, TEXT("CTRL_expressions_mouthCornerPullR"));
    MapARKitCoeff(Coeffs, TEXT("mouthSmile_L"), CtrlCurves, TEXT("CTRL_expressions_mouthCornerUpL"), 0.5f);
    MapARKitCoeff(Coeffs, TEXT("mouthSmile_R"), CtrlCurves, TEXT("CTRL_expressions_mouthCornerUpR"), 0.5f);
    MapARKitCoeff(Coeffs, TEXT("mouthFrown_L"), CtrlCurves, TEXT("CTRL_expressions_mouthCornerDepressL"));
    MapARKitCoeff(Coeffs, TEXT("mouthFrown_R"), CtrlCurves, TEXT("CTRL_expressions_mouthCornerDepressR"));
    MapARKitCoeff(Coeffs, TEXT("mouthDimple_L"), CtrlCurves, TEXT("CTRL_expressions_mouthDimpleL"));
    MapARKitCoeff(Coeffs, TEXT("mouthDimple_R"), CtrlCurves, TEXT("CTRL_expressions_mouthDimpleR"));
    MapARKitCoeff(Coeffs, TEXT("mouthStretch_L"), CtrlCurves, TEXT("CTRL_expressions_mouthStretchL"));
    MapARKitCoeff(Coeffs, TEXT("mouthStretch_R"), CtrlCurves, TEXT("CTRL_expressions_mouthStretchR"));
    MapARKitCoeff(Coeffs, TEXT("mouthRollLower"), CtrlCurves, TEXT("CTRL_expressions_mouthRollLower"));
    MapARKitCoeff(Coeffs, TEXT("mouthRollUpper"), CtrlCurves, TEXT("CTRL_expressions_mouthRollUpper"));
    MapARKitCoeff(Coeffs, TEXT("mouthShrugLower"), CtrlCurves, TEXT("CTRL_expressions_mouthShrugLower"));
    MapARKitCoeff(Coeffs, TEXT("mouthShrugUpper"), CtrlCurves, TEXT("CTRL_expressions_mouthShrugUpper"));
    MapARKitCoeff(Coeffs, TEXT("mouthPress_L"), CtrlCurves, TEXT("CTRL_expressions_mouthPressL"));
    MapARKitCoeff(Coeffs, TEXT("mouthPress_R"), CtrlCurves, TEXT("CTRL_expressions_mouthPressR"));
    MapARKitCoeff(Coeffs, TEXT("mouthLowerDown_L"), CtrlCurves, TEXT("CTRL_expressions_mouthLowerDownL"));
    MapARKitCoeff(Coeffs, TEXT("mouthLowerDown_R"), CtrlCurves, TEXT("CTRL_expressions_mouthLowerDownR"));
    MapARKitCoeff(Coeffs, TEXT("mouthUpperUp_L"), CtrlCurves, TEXT("CTRL_expressions_mouthUpperUpL"));
    MapARKitCoeff(Coeffs, TEXT("mouthUpperUp_R"), CtrlCurves, TEXT("CTRL_expressions_mouthUpperUpR"));

    MapARKitCoeff(Coeffs, TEXT("browDown_L"), CtrlCurves, TEXT("CTRL_expressions_browDownL"));
    MapARKitCoeff(Coeffs, TEXT("browDown_R"), CtrlCurves, TEXT("CTRL_expressions_browDownR"));
    MapARKitCoeff(Coeffs, TEXT("browOuterUp_L"), CtrlCurves, TEXT("CTRL_expressions_browRaiseOuterL"));
    MapARKitCoeff(Coeffs, TEXT("browOuterUp_R"), CtrlCurves, TEXT("CTRL_expressions_browRaiseOuterR"));
    MapARKitCoeff(Coeffs, TEXT("cheekPuff"), CtrlCurves, TEXT("CTRL_expressions_cheekPuff"));
    MapARKitCoeff(Coeffs, TEXT("cheekSquint_L"), CtrlCurves, TEXT("CTRL_expressions_cheekSquintL"));
    MapARKitCoeff(Coeffs, TEXT("cheekSquint_R"), CtrlCurves, TEXT("CTRL_expressions_cheekSquintR"));
    MapARKitCoeff(Coeffs, TEXT("noseSneer_L"), CtrlCurves, TEXT("CTRL_expressions_noseWrinkleL"));
    MapARKitCoeff(Coeffs, TEXT("noseSneer_R"), CtrlCurves, TEXT("CTRL_expressions_noseWrinkleR"));

    float BrowInnerUp = 0.0f;
    if (FindARKitCoeff(Coeffs, TEXT("browInnerUp"), BrowInnerUp))
    {
        AddCtrlCurve(CtrlCurves, TEXT("CTRL_expressions_browRaiseInL"), BrowInnerUp);
        AddCtrlCurve(CtrlCurves, TEXT("CTRL_expressions_browRaiseInR"), BrowInnerUp);
    }

    return CtrlCurves;
}

static void ParseARKitExpressionFrames(
    const TArray<TSharedPtr<FJsonValue>>& ARKitFrames,
    TArray<FMetaHumanExpressionFrame>& OutExpressionFrames)
{
    OutExpressionFrames.Empty();
    for (const TSharedPtr<FJsonValue>& FrameValue : ARKitFrames)
    {
        const TSharedPtr<FJsonObject>* FrameObj = nullptr;
        if (!FrameValue.IsValid() || !FrameValue->TryGetObject(FrameObj) || !FrameObj) continue;

        const TSharedPtr<FJsonObject>* CoeffObj = nullptr;
        const TSharedPtr<FJsonObject>* Candidate = nullptr;
        if ((*FrameObj)->TryGetObjectField(TEXT("coefficients"), Candidate) && Candidate)
        {
            CoeffObj = Candidate;
        }
        else if ((*FrameObj)->TryGetObjectField(TEXT("blendshapes"), Candidate) && Candidate)
        {
            CoeffObj = Candidate;
        }
        else if ((*FrameObj)->TryGetObjectField(TEXT("curves"), Candidate) && Candidate)
        {
            CoeffObj = Candidate;
        }
        else if ((*FrameObj)->TryGetObjectField(TEXT("arkit"), Candidate) && Candidate)
        {
            CoeffObj = Candidate;
        }
        else
        {
            CoeffObj = FrameObj;
        }

        TMap<FName, float> ARKitCurves;
        ParseCurveObject(*CoeffObj, ARKitCurves);
        ARKitCurves.Remove(FName(TEXT("frame")));
        ARKitCurves.Remove(FName(TEXT("time")));
        ARKitCurves.Remove(FName(TEXT("timestamp")));

        FMetaHumanExpressionFrame ExpressionFrame;
        ExpressionFrame.Frame = (*FrameObj)->HasField(TEXT("frame"))
            ? (*FrameObj)->GetIntegerField(TEXT("frame"))
            : OutExpressionFrames.Num();
        ExpressionFrame.Curves = ConvertARKitToMetaHumanCtrlCurves(ARKitCurves);

        if (ExpressionFrame.Curves.Num() > 1)
        {
            OutExpressionFrames.Add(MoveTemp(ExpressionFrame));
        }
    }
}
}

// --- FClientReceiveRunnable implementation ---

FClientReceiveRunnable::FClientReceiveRunnable(FSocket* InClientSocket, TQueue<FCommandData>& InCommandQueue, bool& InShouldRunFlag)
    : ClientSocket(InClientSocket), CommandQueue(InCommandQueue), bShouldRun(InShouldRunFlag)
{
}

bool FClientReceiveRunnable::Init()
{
    return true;
}

void FClientReceiveRunnable::Exit()
{
}

void FClientReceiveRunnable::Stop()
{
    bShouldRun = false;
}

uint32 FClientReceiveRunnable::Run()
{
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(16 * 1024 * 1024); // Allow large ARKit coefficient sequences.
    int32 TotalReceived = 0;

    while (bShouldRun)
    {
        // 只有在缓冲区为空时才等待新数据到来，避免已有数据被跳过
        uint32 PendingSize = 0;
        bool bHasPending = ClientSocket->HasPendingData(PendingSize);

        if (!bHasPending && TotalReceived == 0)
        {
            FPlatformProcess::Sleep(0.01f);
            continue;
        }

        // 有待接收数据，或缓冲区里已有未处理的数据，尝试接收更多
        if (bHasPending)
        {
            int32 Received = 0;
            if (ClientSocket->Recv(Buffer.GetData() + TotalReceived, Buffer.Num() - TotalReceived, Received))
            {
                if (Received > 0)
                {
                    TotalReceived += Received;
                }
                else
                {
                    // 连接已关闭
                    break;
                }
            }
            else
            {
                break;
            }
        }

        // 尝试从缓冲区中解析完整的包
        while (TotalReceived >= (int32)sizeof(FProtocolHeader))
        {
            FProtocolHeader Header;
            FMemory::Memcpy(&Header, Buffer.GetData(), sizeof(FProtocolHeader));
            
            // 自动检测并处理字节序
            bool bNeedsSwap = false;
            if (Header.Magic == 0x52354555) // 检测到字节序翻转
            {
                bNeedsSwap = true;
            }
            else if (Header.Magic != 0x55453552) // 无效 Magic
            {
                // 尝试向后滑动 1 字节以重新同步，而不是直接放弃所有数据
                FMemory::Memmove(Buffer.GetData(), Buffer.GetData() + 1, --TotalReceived);
                continue;
            }

            if (bNeedsSwap)
            {
                Header.PayloadType = ((Header.PayloadType & 0xFF00) >> 8) | ((Header.PayloadType & 0x00FF) << 8);
                Header.PayloadLength = ((Header.PayloadLength & 0xFF000000) >> 24) |
                                       ((Header.PayloadLength & 0x00FF0000) >> 8) |
                                       ((Header.PayloadLength & 0x0000FF00) << 8) |
                                       ((Header.PayloadLength & 0x000000FF) << 24);
            }

            int32 FullSize = sizeof(FProtocolHeader) + Header.PayloadLength;

            if (TotalReceived >= FullSize)
            {
                FCommandData Cmd;
                if (Header.PayloadType == 0 && ParseCommandPayload(Buffer, sizeof(FProtocolHeader), Header.PayloadLength, Cmd))
                {
                    CommandQueue.Enqueue(Cmd);
                    UE_LOG(LogTemp, Warning, TEXT("Command received: %s for actor: %s (Payload: %d bytes)"), *Cmd.Cmd, *Cmd.ActorName, Header.PayloadLength);
                }
                else
                {
                    UE_LOG(LogTemp, Error, TEXT("Failed to parse command payload of length %d"), Header.PayloadLength);
                }

                int32 Remaining = TotalReceived - FullSize;
                if (Remaining > 0)
                {
                    FMemory::Memmove(Buffer.GetData(), Buffer.GetData() + FullSize, Remaining);
                }
                TotalReceived = Remaining;
            }
            else
            {
                // 包不完整，等待更多数据
                break;
            }
        }
    }
    return 0;

}

bool FClientReceiveRunnable::ParseCommandPayload(const TArray<uint8>& Data, int32 Offset, int32 Length, FCommandData& OutCmd)
{
    if (Length <= 0 || Offset + Length > Data.Num()) return false;

    FString JsonStr;
    FUTF8ToTCHAR Convert(reinterpret_cast<const ANSICHAR*>(Data.GetData() + Offset), Length);
    JsonStr = FString(Convert.Length(), Convert.Get());

    TSharedPtr<FJsonObject> Json;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);

    if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
    {
        if (Json->HasField(TEXT("cmd")))
        {
            OutCmd.Cmd = Json->GetStringField(TEXT("cmd"));
        }
        else if (Json->HasField(TEXT("lights")))
        {
            OutCmd.Cmd = TEXT("create_lights");
        }
        
        OutCmd.ActorName = Json->HasField(TEXT("actor")) ? Json->GetStringField(TEXT("actor")) : TEXT("World");

        // 如果命令是 create_lights 或包含 lights 字段
        if (OutCmd.Cmd == TEXT("create_lights") || Json->HasField(TEXT("lights")))
        {
            FLightManager::ParseLights(Json, nullptr, OutCmd.Lights);
        }

        const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;

        if (Json->TryGetArrayField(TEXT("pos"), Pos) && Pos->Num() == 3)
        {
            OutCmd.Position = FVector((*Pos)[0]->AsNumber(), (*Pos)[1]->AsNumber(), (*Pos)[2]->AsNumber());
        }

        if (Json->TryGetArrayField(TEXT("rot"), Rot) && Rot->Num() == 4)
        {
            OutCmd.Rotation = FQuat((*Rot)[0]->AsNumber(), (*Rot)[1]->AsNumber(), (*Rot)[2]->AsNumber(), (*Rot)[3]->AsNumber());
        }

        if (Json->HasField(TEXT("export_path"))) OutCmd.ExportPath = Json->GetStringField(TEXT("export_path"));
        if (Json->HasField(TEXT("frame_number"))) OutCmd.FrameNumber = Json->GetIntegerField(TEXT("frame_number"));
        if (Json->HasField(TEXT("component"))) OutCmd.ComponentName = Json->GetStringField(TEXT("component"));
        if (Json->HasField(TEXT("rgb_mat"))) OutCmd.RGBMaterialPath = Json->GetStringField(TEXT("rgb_mat"));
        if (Json->HasField(TEXT("depth_mat"))) OutCmd.DepthMaterialPath = Json->GetStringField(TEXT("depth_mat"));
        if (Json->HasField(TEXT("pos_mat"))) OutCmd.PositionMaterialPath = Json->GetStringField(TEXT("pos_mat"));
        if (Json->HasField(TEXT("cubemap_res"))) OutCmd.CubemapResolution = Json->GetIntegerField(TEXT("cubemap_res"));
        if (Json->HasField(TEXT("use_cubemap"))) OutCmd.bUseCubemap = Json->GetBoolField(TEXT("use_cubemap"));
        if (Json->HasField(TEXT("exposure_bias"))) OutCmd.ExposureBias = Json->GetNumberField(TEXT("exposure_bias"));
        if (Json->HasField(TEXT("enable_tone_curve"))) OutCmd.bEnableToneCurve = Json->GetBoolField(TEXT("enable_tone_curve"));
        if (Json->HasField(TEXT("use_hdr"))) OutCmd.bUseHDR = Json->GetBoolField(TEXT("use_hdr"));
        if (Json->HasField(TEXT("warmup_frames"))) OutCmd.WarmUpFrames = Json->GetIntegerField(TEXT("warmup_frames"));
        if (Json->HasField(TEXT("supersampling"))) OutCmd.SupersamplingMultiplier = Json->GetNumberField(TEXT("supersampling"));

        if (Json->HasField(TEXT("resolution")))
        {
            const TArray<TSharedPtr<FJsonValue>>* Res = nullptr;
            if (Json->TryGetArrayField(TEXT("resolution"), Res) && Res->Num() == 2)
            {
                OutCmd.Resolution = FIntPoint((*Res)[0]->AsNumber(), (*Res)[1]->AsNumber());
            }
        }

        if (Json->HasField(TEXT("model"))) OutCmd.CameraModel = Json->GetStringField(TEXT("model"));

        if (Json->HasField(TEXT("intrinsics")))
        {
            const TArray<TSharedPtr<FJsonValue>>* Intr = nullptr;
            if (Json->TryGetArrayField(TEXT("intrinsics"), Intr))
            {
                OutCmd.Intrinsics.Empty();
                for (auto& Val : *Intr) OutCmd.Intrinsics.Add(Val->AsNumber());
            }
        }

        if (Json->HasField(TEXT("distortion")))
        {
            const TArray<TSharedPtr<FJsonValue>>* Dist = nullptr;
            if (Json->TryGetArrayField(TEXT("distortion"), Dist))
            {
                OutCmd.DistortionCoeffs.Empty();
                for (auto& Val : *Dist) OutCmd.DistortionCoeffs.Add(Val->AsNumber());
            }
        }
        
        // 手动解析 MultiView 参数 (修复 snake_case 匹配问题)
        if (Json->HasField(TEXT("angle_step"))) OutCmd.AngleStep = Json->GetNumberField(TEXT("angle_step"));
        if (Json->HasField(TEXT("horizontal_fov"))) OutCmd.HorizontalFOV = Json->GetNumberField(TEXT("horizontal_fov"));
        if (Json->HasField(TEXT("vertical_fov"))) OutCmd.VerticalFOV = Json->GetNumberField(TEXT("vertical_fov"));
        
        if (Json->HasField(TEXT("render_mode"))) OutCmd.RenderMode = (EMultiViewRenderMode)Json->GetIntegerField(TEXT("render_mode"));
        if (Json->HasField(TEXT("num_parallel_probes"))) OutCmd.NumParallelProbes = Json->GetIntegerField(TEXT("num_parallel_probes"));

        // ── Dome LightStage 参数解析 ──
        if (Json->HasField(TEXT("dome_radius")))         OutCmd.DomeRadius = Json->GetNumberField(TEXT("dome_radius"));
        if (Json->HasField(TEXT("dome_num_cameras")))    OutCmd.DomeNumCameras = Json->GetIntegerField(TEXT("dome_num_cameras"));
        if (Json->HasField(TEXT("dome_layout")))         OutCmd.DomeLayout = Json->GetStringField(TEXT("dome_layout"));
        if (Json->HasField(TEXT("dome_fov")))            OutCmd.DomeCameraFOV = Json->GetNumberField(TEXT("dome_fov"));
        if (Json->HasField(TEXT("dome_batch_size")))     OutCmd.DomeRenderBatchSize = Json->GetIntegerField(TEXT("dome_batch_size"));
        if (Json->HasField(TEXT("dome_enabled_passes"))) OutCmd.DomeEnabledPasses = (uint8)Json->GetIntegerField(TEXT("dome_enabled_passes"));
        if (Json->HasField(TEXT("dome_upper_hemisphere_only"))) OutCmd.bDomeUpperHemisphereOnly = Json->GetBoolField(TEXT("dome_upper_hemisphere_only"));
        if (Json->HasField(TEXT("normal_mat")))          OutCmd.NormalMaterialPath = Json->GetStringField(TEXT("normal_mat"));
        if (Json->HasField(TEXT("semantic_mat")))        OutCmd.SemanticMaterialPath = Json->GetStringField(TEXT("semantic_mat"));

        // Dome Ring 模式数组
        const TArray<TSharedPtr<FJsonValue>>* RingCams = nullptr;
        if (Json->TryGetArrayField(TEXT("dome_cameras_per_ring"), RingCams))
        {
            OutCmd.DomeCamerasPerRing.Empty();
            for (auto& Val : *RingCams) OutCmd.DomeCamerasPerRing.Add((int32)Val->AsNumber());
        }
        const TArray<TSharedPtr<FJsonValue>>* RingElevs = nullptr;
        if (Json->TryGetArrayField(TEXT("dome_ring_elevations"), RingElevs))
        {
            OutCmd.DomeRingElevations.Empty();
            for (auto& Val : *RingElevs) OutCmd.DomeRingElevations.Add(Val->AsNumber());
        }

        // Dome Manual 模式：手动相机列表
        // JSON 格式: "dome_manual_cameras": [
        //   {"name": "cam0", "pos": [x,y,z], "rot": [pitch,yaw,roll], "fov": 50},
        //   {"name": "cam1", "pos": [x,y,z], "rot_quat": [x,y,z,w], "fov": 35}, ...]
        const TArray<TSharedPtr<FJsonValue>>* ManualCams = nullptr;
        if (Json->TryGetArrayField(TEXT("dome_manual_cameras"), ManualCams))
        {
            OutCmd.DomeManualCameras.Empty();
            for (int32 ci = 0; ci < ManualCams->Num(); ci++)
            {
                const TSharedPtr<FJsonObject>* CamObjPtr = nullptr;
                if (!(*ManualCams)[ci]->TryGetObject(CamObjPtr) || !CamObjPtr) continue;
                const TSharedPtr<FJsonObject>& CamObj = *CamObjPtr;

                FDomeCameraView View;
                View.CameraIndex = ci;
                View.CameraName = CamObj->HasField(TEXT("name")) ? CamObj->GetStringField(TEXT("name")) : FString::Printf(TEXT("ManualCam_%03d"), ci);
                View.FOV = CamObj->HasField(TEXT("fov")) ? CamObj->GetNumberField(TEXT("fov")) : OutCmd.DomeCameraFOV;

                // 位置: "pos": [x, y, z] (相对 dome 中心)
                const TArray<TSharedPtr<FJsonValue>>* PosArr = nullptr;
                if (CamObj->TryGetArrayField(TEXT("pos"), PosArr) && PosArr->Num() == 3)
                    View.RelativePosition = FVector((*PosArr)[0]->AsNumber(), (*PosArr)[1]->AsNumber(), (*PosArr)[2]->AsNumber());

                // 朝向: "rot": [pitch, yaw, roll] (欧拉角, 度)
                const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
                if (CamObj->TryGetArrayField(TEXT("rot"), RotArr) && RotArr->Num() == 3)
                    View.LookAtRotation = FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());

                // 或者用四元数: "rot_quat": [x, y, z, w]
                const TArray<TSharedPtr<FJsonValue>>* QuatArr = nullptr;
                if (CamObj->TryGetArrayField(TEXT("rot_quat"), QuatArr) && QuatArr->Num() == 4)
                {
                    FQuat Q((*QuatArr)[0]->AsNumber(), (*QuatArr)[1]->AsNumber(), (*QuatArr)[2]->AsNumber(), (*QuatArr)[3]->AsNumber());
                    View.LookAtRotation = Q.Rotator();
                }

                OutCmd.DomeManualCameras.Add(View);
            }
            UE_LOG(LogTemp, Log, TEXT("Parsed %d manual dome cameras"), OutCmd.DomeManualCameras.Num());
        }

        if (Json->HasField(TEXT("capture_actor"))) OutCmd.CaptureActorName = Json->GetStringField(TEXT("capture_actor"));
        if (Json->HasField(TEXT("capture_component"))) OutCmd.CaptureComponentName = Json->GetStringField(TEXT("capture_component"));
        if (Json->HasField(TEXT("frame_count"))) OutCmd.SequenceFrameCount = Json->GetIntegerField(TEXT("frame_count"));
        if (Json->HasField(TEXT("sequence_actor"))) OutCmd.SequenceActorName = Json->GetStringField(TEXT("sequence_actor"));
        if (OutCmd.SequenceActorName.IsEmpty() && Json->HasField(TEXT("sequence"))) OutCmd.SequenceActorName = Json->GetStringField(TEXT("sequence"));
        if (OutCmd.SequenceActorName.IsEmpty() && Json->HasField(TEXT("level_sequence_actor"))) OutCmd.SequenceActorName = Json->GetStringField(TEXT("level_sequence_actor"));
        if (Json->HasField(TEXT("start_frame"))) OutCmd.StartFrame = Json->GetIntegerField(TEXT("start_frame"));
        if (Json->HasField(TEXT("end_frame"))) OutCmd.EndFrame = Json->GetIntegerField(TEXT("end_frame"));
        if (Json->HasField(TEXT("frame_step"))) OutCmd.FrameStep = Json->GetIntegerField(TEXT("frame_step"));
        if (Json->HasField(TEXT("sequence_wait_frames"))) OutCmd.EvaluationWaitFrames = Json->GetIntegerField(TEXT("sequence_wait_frames"));
        if (Json->HasField(TEXT("render_after_seek"))) OutCmd.bRenderAfterSequenceSeek = Json->GetBoolField(TEXT("render_after_seek"));

        const TSharedPtr<FJsonObject>* CurvesObj = nullptr;
        if (Json->TryGetObjectField(TEXT("curves"), CurvesObj) && CurvesObj)
        {
            ParseCurveObject(*CurvesObj, OutCmd.MetaHumanCurves);
        }

        const TArray<TSharedPtr<FJsonValue>>* ExpressionFrames = nullptr;
        if (Json->TryGetArrayField(TEXT("expression_frames"), ExpressionFrames))
        {
            OutCmd.MetaHumanExpressionFrames.Empty();
            for (const TSharedPtr<FJsonValue>& FrameValue : *ExpressionFrames)
            {
                const TSharedPtr<FJsonObject>* FrameObj = nullptr;
                if (!FrameValue.IsValid() || !FrameValue->TryGetObject(FrameObj) || !FrameObj) continue;

                FMetaHumanExpressionFrame ExpressionFrame;
                ExpressionFrame.Frame = (*FrameObj)->HasField(TEXT("frame")) ? (*FrameObj)->GetIntegerField(TEXT("frame")) : OutCmd.MetaHumanExpressionFrames.Num();

                const TSharedPtr<FJsonObject>* FrameCurves = nullptr;
                if ((*FrameObj)->TryGetObjectField(TEXT("curves"), FrameCurves) && FrameCurves)
                {
                    ParseCurveObject(*FrameCurves, ExpressionFrame.Curves);
                }

                if (ExpressionFrame.Curves.Num() > 0)
                {
                    OutCmd.MetaHumanExpressionFrames.Add(MoveTemp(ExpressionFrame));
                }
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* ARKitExpressionFrames = nullptr;
        if (Json->TryGetArrayField(TEXT("arkit_expression_frames"), ARKitExpressionFrames) ||
            Json->TryGetArrayField(TEXT("arkit_frames"), ARKitExpressionFrames) ||
            Json->TryGetArrayField(TEXT("arkit_blendshape_frames"), ARKitExpressionFrames))
        {
            ParseARKitExpressionFrames(*ARKitExpressionFrames, OutCmd.MetaHumanExpressionFrames);
            OutCmd.MetaHumanExpressionFrames.Sort([](const FMetaHumanExpressionFrame& A, const FMetaHumanExpressionFrame& B)
            {
                return A.Frame < B.Frame;
            });
            UE_LOG(LogTemp, Warning, TEXT("[ARKit] Converted %d ARKit frames to CTRL_* curves"),
                OutCmd.MetaHumanExpressionFrames.Num());
        }

        UE_LOG(LogTemp, Log, TEXT("Parsed Command: AngleStep=%.2f, HFOV=%.2f, VFOV=%.2f, Mode=%d, Probes=%d"), 
            OutCmd.AngleStep, OutCmd.HorizontalFOV, OutCmd.VerticalFOV, (int32)OutCmd.RenderMode, OutCmd.NumParallelProbes);

        return true;
    }
    
    // 尝试解析为数组 (兼容直接发送灯光列表)
    TArray<TSharedPtr<FJsonValue>> JsonArray;
    Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (FJsonSerializer::Deserialize(Reader, JsonArray))
    {
        OutCmd.Cmd = TEXT("create_lights");
        OutCmd.ActorName = TEXT("World");
        FLightManager::ParseLights(nullptr, &JsonArray, OutCmd.Lights);
        UE_LOG(LogTemp, Log, TEXT("Parsed %d lights from root-level JSON array"), OutCmd.Lights.Num());
        return true;
    }

    return false;
}

// --- ARenderServer implementation ---

ARenderServer::ARenderServer()
{
    PrimaryActorTick.bCanEverTick = true;
}

// 修复 MetaHuman Face Semantic 材质缺少 bUsedWithMorphTargets 的问题。
// 当材质未开启该标志时，UE 会将 Face mesh 换成默认材质，导致变形不可见。
// 这里在运行时对 SkeletalMesh 的材质实例强制打上标志，避免重编译材质资产。
static void FixMetaHumanMorphMaterials(AActor* TargetActor)
{
    if (!TargetActor) return;

    TArray<USkeletalMeshComponent*> SkelComps;
    TargetActor->GetComponents<USkeletalMeshComponent>(SkelComps, true);

    for (USkeletalMeshComponent* SkelComp : SkelComps)
    {
        if (!SkelComp || !SkelComp->GetSkeletalMeshAsset()) continue;

        const FString CompName  = SkelComp->GetName();
        const FString AssetName = SkelComp->GetSkeletalMeshAsset()->GetName();
        if (!CompName.Contains(TEXT("Face"), ESearchCase::IgnoreCase) &&
            !AssetName.Contains(TEXT("face"), ESearchCase::IgnoreCase))
        {
            continue;
        }

        // 对 Face 组件的每个材质槽，若材质未开启 MorphTargets，创建动态实例并强制开启
        const int32 NumMats = SkelComp->GetNumMaterials();
        for (int32 mi = 0; mi < NumMats; ++mi)
        {
            UMaterialInterface* Mat = SkelComp->GetMaterial(mi);
            if (!Mat) continue;

            UMaterial* BaseMat = Mat->GetMaterial();
            if (!BaseMat || BaseMat->bUsedWithMorphTargets) continue;

            // 创建动态材质实例，不修改原始资产
            UMaterialInstanceDynamic* MID = SkelComp->CreateAndSetMaterialInstanceDynamic(mi);
            if (MID)
            {
                // 通过修改父材质标志让该实例支持 Morph（运行时有效，不影响资产文件）
                MID->GetMaterial()->bUsedWithMorphTargets = true;
                UE_LOG(LogTemp, Log, TEXT("[MetaHuman] Fixed MorphTarget flag on mat slot %d of '%s'"),
                    mi, *CompName);
            }
        }
    }
}

void ARenderServer::BeginPlay()
{
    Super::BeginPlay();

    FIPv4Endpoint Endpoint(FIPv4Address::Any, ListenPort);
    TcpListener = new FTcpListener(Endpoint);
    TcpListener->OnConnectionAccepted().BindUObject(this, &ARenderServer::OnConnectionAccepted);

    UE_LOG(LogTemp, Warning, TEXT("RenderServer listening on port %d"), ListenPort);
}

bool ARenderServer::OnConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& InEndpoint)
{
    UE_LOG(LogTemp, Warning, TEXT("Client connected from %s"), *InEndpoint.ToString());

    if (ClientSocket)
    {
        bShouldRunClientReceiveThread = false;
        if (ClientReceiveThread)
        {
            ClientReceiveThread->Kill(true);
            delete ClientReceiveThread;
            ClientReceiveThread = nullptr;
        }
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
    }

    ClientSocket = InSocket;
    bShouldRunClientReceiveThread = true;

    FClientReceiveRunnable* Runnable = new FClientReceiveRunnable(ClientSocket, IncomingCommands, bShouldRunClientReceiveThread);
    ClientReceiveThread = FRunnableThread::Create(Runnable, TEXT("ClientReceiveThread"), 0, TPri_Normal);

    return true;
}

AActor* ARenderServer::FindActorByNameOrLabel(const FString& TargetName) const
{
    if (!GetWorld() || TargetName.IsEmpty()) return nullptr;

    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        if (It->GetActorNameOrLabel().Equals(TargetName, ESearchCase::IgnoreCase) ||
            It->GetName().Contains(TargetName, ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }
    return nullptr;
}

ALevelSequenceActor* ARenderServer::FindLevelSequenceActorByNameOrLabel(const FString& TargetName) const
{
    if (!GetWorld() || TargetName.IsEmpty()) return nullptr;

    for (TActorIterator<ALevelSequenceActor> It(GetWorld()); It; ++It)
    {
        if (It->GetActorNameOrLabel().Equals(TargetName, ESearchCase::IgnoreCase) ||
            It->GetName().Contains(TargetName, ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }

    return Cast<ALevelSequenceActor>(FindActorByNameOrLabel(TargetName));
}

bool ARenderServer::ApplyMetaHumanCurves(AActor* TargetActor, const TMap<FName, float>& Curves) const
{
    if (!TargetActor || Curves.Num() == 0) return false;

    // 首次调用时修复 Face 材质缺少 bUsedWithMorphTargets 的问题（见日志里的 Warning）
    FixMetaHumanMorphMaterials(TargetActor);

    TArray<USkeletalMeshComponent*> SkelComps;
    TargetActor->GetComponents<USkeletalMeshComponent>(SkelComps, true);

    int32 AppliedComponents = 0;
    for (USkeletalMeshComponent* SkelComp : SkelComps)
    {
        if (!IsMetaHumanFaceComponent(SkelComp)) continue;

        PrepareFaceForRigLogicCapture(SkelComp);

        float JawOpen = -1.f;
        int32 CtrlCount = 0;

        TArray<UAnimInstance*> AnimTargets;
        CollectFaceAnimInstances(SkelComp, AnimTargets);

        UAnimInstance* MainInst = SkelComp->GetAnimInstance();
        UAnimInstance* PostProcessInst = SkelComp->GetPostProcessInstance();

        for (UAnimInstance* TargetInst : AnimTargets)
        {
            CtrlCount += InjectRigLogicCurvesOnAnimInstance(TargetInst, Curves, &JawOpen);
        }

        ForceFaceRigLogicEvaluation(SkelComp, AnimTargets, Curves);

        float JawReadBack = -1.f;
        if (PostProcessInst)
        {
            PostProcessInst->GetCurveValue(FName(TEXT("CTRL_expressions_jawOpen")), JawReadBack);
        }

        ++AppliedComponents;
        UE_LOG(LogTemp, Log, TEXT("[MetaHuman] Override %d CTRL on Face '%s' (jawOpen=%.3f, readback=%.3f, Main=%s, PP=%s)"),
            CtrlCount, *SkelComp->GetName(), JawOpen, JawReadBack,
            MainInst ? *MainInst->GetClass()->GetName() : TEXT("MISSING"),
            PostProcessInst ? *PostProcessInst->GetClass()->GetName() : TEXT("MISSING"));

        if (!PostProcessInst)
        {
            UE_LOG(LogTemp, Error,
                TEXT("[MetaHuman] Face '%s' has NO PostProcess AnimInstance — RigLogic will not run. "
                     "Assign Face_PostProcess_AnimBP on the Face SkeletalMeshComponent."),
                *SkelComp->GetName());
        }
    }

    if (AppliedComponents == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[MetaHuman] No Face component on '%s'"), *TargetActor->GetActorNameOrLabel());
    }

    return AppliedComponents > 0;
}

// bool ARenderServer::ApplyMetaHumanCurves(AActor* TargetActor, const TMap<FName, float>& Curves) const
// {
//     if (!TargetActor) return false;

//     FixMetaHumanMorphMaterials(TargetActor);

//     TArray<USkeletalMeshComponent*> SkelComps;
//     TargetActor->GetComponents<USkeletalMeshComponent>(SkelComps, true);

//     int32 AppliedComponents = 0;
//     for (USkeletalMeshComponent* SkelComp : SkelComps)
//     {
//         if (!IsMetaHumanFaceComponent(SkelComp)) continue;

//         // 1. 清除上一帧残留的覆盖值
//         SkelComp->ClearMorphTargets();

//         if (Curves.Num() > 0)
//         {
//             // 2. 冻结引擎自然 Tick，防止表情在渲染前或渲染中发生偏移抖动
//             SkelComp->bPauseAnims = true;

//             // 3. 核心机制：利用底层的 MorphTarget API 强行覆盖 Animation Curves
//             // 此时 Control Rig 的 0.0 输出将被完全无视，RigLogic 将直接读取这里的数值
//             for (const TPair<FName, float>& Curve : Curves)
//             {
//                 SkelComp->SetMorphTarget(Curve.Key, Curve.Value);
//             }
//         }
//         else
//         {
//             // 如果传入空曲线，恢复自然动画
//             SkelComp->bPauseAnims = false;
//         }

//         // 4. 强制引擎立刻使用新的曲线池重新解算骨骼姿态
//         SkelComp->TickAnimation(1.0f / 30.0f, false);
//         SkelComp->RefreshBoneTransforms();
//         SkelComp->FinalizeBoneTransform();
        
//         // 5. 标记渲染系统重绘
//         SkelComp->MarkRenderTransformDirty();
//         SkelComp->MarkRenderDynamicDataDirty();

//         ++AppliedComponents;
        
//         UE_LOG(LogTemp, Log, TEXT("[MetaHuman] Injected %d curves via Component Curve Map on '%s'"), Curves.Num(), *SkelComp->GetName());
//     }

//     return AppliedComponents > 0;
// }

void ARenderServer::SetMetaHumanAnimPaused(AActor* TargetActor, bool bPause) const
{
    // 新策略下不再 pause AnimBP，保留此函数仅用于紧急情况或序列异常中止时清理。
    // 正常序列流程中不会调用此函数暂停，只会在序列结束时调用 bPause=false 做保险性恢复。
    if (!TargetActor) return;

    TArray<USkeletalMeshComponent*> SkelComps;
    TargetActor->GetComponents<USkeletalMeshComponent>(SkelComps, true);
    for (USkeletalMeshComponent* SkelComp : SkelComps)
    {
        if (SkelComp)
        {
            SkelComp->bPauseAnims = bPause;
            if (!bPause) SkelComp->RefreshBoneTransforms();
        }
    }
    UE_LOG(LogTemp, Log, TEXT("[MetaHuman] AnimBP %s on '%s'"),
        bPause ? TEXT("PAUSED") : TEXT("RESUMED"), *TargetActor->GetActorNameOrLabel());
}

TMap<FName, float> ARenderServer::EvaluateMetaHumanCurvesAtFrame(
    const TArray<FMetaHumanExpressionFrame>& Keyframes, int32 Frame) const
{
    TMap<FName, float> Result;
    if (Keyframes.Num() == 0) return Result;
    if (Keyframes.Num() == 1 || Frame <= Keyframes[0].Frame) return Keyframes[0].Curves;

    for (int32 i = 1; i < Keyframes.Num(); ++i)
    {
        const FMetaHumanExpressionFrame& Prev = Keyframes[i - 1];
        const FMetaHumanExpressionFrame& Next = Keyframes[i];
        if (Frame > Next.Frame) continue;

        const float Alpha = (Next.Frame == Prev.Frame)
            ? 1.0f
            : FMath::Clamp((Frame - Prev.Frame) / float(Next.Frame - Prev.Frame), 0.0f, 1.0f);

        TSet<FName> Names;
        for (const TPair<FName, float>& Curve : Prev.Curves) Names.Add(Curve.Key);
        for (const TPair<FName, float>& Curve : Next.Curves) Names.Add(Curve.Key);

        for (const FName& Name : Names)
        {
            const float A = Prev.Curves.FindRef(Name);
            const float B = Next.Curves.FindRef(Name);
            Result.Add(Name, FMath::Lerp(A, B, Alpha));
        }
        return Result;
    }

    return Keyframes.Last().Curves;
}

bool ARenderServer::CaptureDomeFrame(
    AActor* CaptureActor, const FString& ComponentName,
    const FString& ExportPath, int32 FrameNumber)
{
    if (ActiveMetaHumanSequence.bActive)
    {
        if (AActor* MetaHumanActor = FindActorByNameOrLabel(ActiveMetaHumanSequence.MetaHumanActorName))
        {
            const TMap<FName, float> Curves = EvaluateMetaHumanCurvesAtFrame(
                ActiveMetaHumanSequence.Keyframes, FrameNumber);
            ApplyMetaHumanCurves(MetaHumanActor, Curves);
            FlushRenderingCommands();
        }
    }

    if (!CaptureActor || ExportPath.IsEmpty()) return false;

    TArray<UDomeLightStageCameraComponent*> DomeCameras;
    CaptureActor->GetComponents<UDomeLightStageCameraComponent>(DomeCameras, true);
    for (UDomeLightStageCameraComponent* DomeCam : DomeCameras)
    {
        if (!DomeCam) continue;
        if (!ComponentName.IsEmpty() && DomeCam->GetName() != ComponentName) continue;
        return DomeCam->SaveAllData(ExportPath, FrameNumber);
    }

    UE_LOG(LogTemp, Error, TEXT("[RenderServer] No DomeLightStageCameraComponent found on %s (requested component='%s')"),
        *CaptureActor->GetActorNameOrLabel(), *ComponentName);
    return false;
}

bool ARenderServer::SetLevelSequenceFrame(const FString& SequenceActorName, int32 FrameNumber) const
{
    ALevelSequenceActor* SequenceActor = FindLevelSequenceActorByNameOrLabel(SequenceActorName);
    if (!SequenceActor)
    {
        UE_LOG(LogTemp, Error, TEXT("[LevelSequence] Actor '%s' not found"), *SequenceActorName);
        return false;
    }

    ULevelSequencePlayer* Player = SequenceActor->GetSequencePlayer();
    ULevelSequence* Sequence = SequenceActor->GetSequence();
    if (!Player || !Sequence)
    {
        UE_LOG(LogTemp, Error, TEXT("[LevelSequence] Actor '%s' has no player or sequence asset"), *SequenceActorName);
        return false;
    }

    Player->Pause();

    FMovieSceneSequencePlaybackParams Params(FFrameTime(FrameNumber), EUpdatePositionMethod::Jump);
    Params.PositionType = EMovieScenePositionType::Frame;
    Params.UpdateMethod = EUpdatePositionMethod::Jump;
    Player->SetPlaybackPosition(Params);
    Player->Pause();

    UE_LOG(LogTemp, Log, TEXT("[LevelSequence] %s -> display frame %d"),
        *SequenceActor->GetActorNameOrLabel(), FrameNumber);
    return true;
}

int32 ARenderServer::ResolveLevelSequenceEndFrame(
    const FString& SequenceActorName, int32 StartFrame, int32 RequestedEndFrame, int32 FrameCount) const
{
    if (FrameCount > 0)
    {
        return StartFrame + FrameCount - 1;
    }
    if (RequestedEndFrame >= StartFrame)
    {
        return RequestedEndFrame;
    }

    if (ALevelSequenceActor* SequenceActor = FindLevelSequenceActorByNameOrLabel(SequenceActorName))
    {
        if (ULevelSequencePlayer* Player = SequenceActor->GetSequencePlayer())
        {
            const int32 Duration = Player->GetFrameDuration();
            if (Duration > 0)
            {
                return StartFrame + Duration - 1;
            }
        }

        if (ULevelSequence* Sequence = SequenceActor->GetSequence())
        {
            if (UMovieScene* MovieScene = Sequence->GetMovieScene())
            {
                const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
                if (PlaybackRange.HasUpperBound())
                {
                    return PlaybackRange.GetUpperBoundValue().Value - 1;
                }
            }
        }
    }

    return StartFrame;
}

bool ARenderServer::CaptureLevelSequenceFrame(const FLevelSequenceCaptureState& State, int32 FrameNumber)
{
    if (!State.bRenderAfterSeek)
    {
        return true;
    }

    AActor* CaptureActor = FindActorByNameOrLabel(State.CaptureActorName);
    if (!CaptureActor)
    {
        UE_LOG(LogTemp, Error, TEXT("[LevelSequence] Capture actor '%s' not found"), *State.CaptureActorName);
        return false;
    }

    FlushRenderingCommands();
    return CaptureDomeFrame(CaptureActor, State.CaptureComponentName, State.ExportPath, FrameNumber);
}

void ARenderServer::TickLevelSequenceCapture()
{
    if (!ActiveLevelSequenceCapture.bActive) return;

    const int32 Frame = ActiveLevelSequenceCapture.CurrentFrame;

    if (ActiveLevelSequenceCapture.WaitFramesRemaining < 0)
    {
        const bool bSeekOK = SetLevelSequenceFrame(ActiveLevelSequenceCapture.SequenceActorName, Frame);
        if (!bSeekOK)
        {
            ActiveLevelSequenceCapture.bActive = false;
            return;
        }

        if (ActiveLevelSequenceCapture.FaceOverrideKeyframes.Num() > 0)
        {
            if (AActor* MetaHumanActor = FindActorByNameOrLabel(ActiveLevelSequenceCapture.MetaHumanActorName))
            {
                PrepareMetaHumanForExternalFaceOverride(MetaHumanActor);
                const TMap<FName, float> Curves = EvaluateMetaHumanCurvesAtFrame(
                    ActiveLevelSequenceCapture.FaceOverrideKeyframes, Frame);
                ApplyMetaHumanCurves(MetaHumanActor, Curves);
                UE_LOG(LogTemp, Log, TEXT("[LevelSequence][ARKit] Frame %d: CTRL_* override applied"), Frame);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("[LevelSequence][ARKit] MetaHuman actor '%s' not found"),
                    *ActiveLevelSequenceCapture.MetaHumanActorName);
            }
        }

        ActiveLevelSequenceCapture.WaitFramesRemaining = FMath::Max(0, ActiveLevelSequenceCapture.EvaluationWaitFrames);
        return;
    }

    if (ActiveLevelSequenceCapture.WaitFramesRemaining > 0)
    {
        --ActiveLevelSequenceCapture.WaitFramesRemaining;
        return;
    }

    if (ActiveLevelSequenceCapture.FaceOverrideKeyframes.Num() > 0)
    {
        if (AActor* MetaHumanActor = FindActorByNameOrLabel(ActiveLevelSequenceCapture.MetaHumanActorName))
        {
            PrepareMetaHumanForExternalFaceOverride(MetaHumanActor);
            const TMap<FName, float> Curves = EvaluateMetaHumanCurvesAtFrame(
                ActiveLevelSequenceCapture.FaceOverrideKeyframes, Frame);
            ApplyMetaHumanCurves(MetaHumanActor, Curves);
            FlushRenderingCommands();
        }
    }

    const bool bCaptureOK = CaptureLevelSequenceFrame(ActiveLevelSequenceCapture, Frame);
    UE_LOG(LogTemp, Log, TEXT("[LevelSequence] Frame %d captured: %s"),
        Frame, bCaptureOK ? TEXT("OK") : TEXT("FAILED"));

    ActiveLevelSequenceCapture.WaitFramesRemaining = -1;
    ActiveLevelSequenceCapture.CurrentFrame += FMath::Max(1, ActiveLevelSequenceCapture.FrameStep);

    if (ActiveLevelSequenceCapture.CurrentFrame > ActiveLevelSequenceCapture.EndFrame)
    {
        ActiveLevelSequenceCapture.bActive = false;
        UE_LOG(LogTemp, Warning, TEXT("[LevelSequence] Capture DONE: %s frames %d..%d -> %s"),
            *ActiveLevelSequenceCapture.SequenceActorName,
            ActiveLevelSequenceCapture.CurrentFrame,
            ActiveLevelSequenceCapture.EndFrame,
            *ActiveLevelSequenceCapture.ExportPath);
    }
}

void ARenderServer::TickMetaHumanSequence()
{
    if (!ActiveMetaHumanSequence.bActive) return;

    AActor* MetaHumanActor = FindActorByNameOrLabel(ActiveMetaHumanSequence.MetaHumanActorName);
    AActor* CaptureActor   = FindActorByNameOrLabel(ActiveMetaHumanSequence.CaptureActorName);
    if (!MetaHumanActor || !CaptureActor)
    {
        UE_LOG(LogTemp, Error, TEXT("[MetaHuman] Sequence aborted. MetaHuman=%s CaptureActor=%s"),
            MetaHumanActor ? TEXT("OK") : TEXT("MISSING"),
            CaptureActor   ? TEXT("OK") : TEXT("MISSING"));
        // 确保恢复动画再退出
        if (MetaHumanActor && ActiveMetaHumanSequence.bAnimPaused)
        {
            SetMetaHumanAnimPaused(MetaHumanActor, false);
            ActiveMetaHumanSequence.bAnimPaused = false;
        }
        ActiveMetaHumanSequence.bActive = false;
        return;
    }

    const int32 Frame = ActiveMetaHumanSequence.CurrentFrame;

    // ── 阶段 1：WaitFramesRemaining == -1，还没有对本帧 Apply 曲线 ──────────
    if (ActiveMetaHumanSequence.WaitFramesRemaining < 0)
    {
        TMap<FName, float> Curves = EvaluateMetaHumanCurvesAtFrame(
            ActiveMetaHumanSequence.Keyframes, Frame);

        // ApplyMetaHumanCurves：注入 curve 值到 Face AnimInstance，AnimBP 保持运行。
        // bAnimPaused 保持 false（新策略不 pause AnimBP）。
        ApplyMetaHumanCurves(MetaHumanActor, Curves);
        ActiveMetaHumanSequence.bAnimPaused = false;
        ActiveMetaHumanSequence.WaitFramesRemaining = 2;

        UE_LOG(LogTemp, Log, TEXT("[MetaHuman] Frame %d: RigLogic curves applied, waiting 2 ticks..."), Frame);
        return;
    }

    // ── 阶段 2：WaitFramesRemaining > 0，倒计时等 GPU 稳定 ─────────────────
    if (ActiveMetaHumanSequence.WaitFramesRemaining > 0)
    {
        --ActiveMetaHumanSequence.WaitFramesRemaining;
        return;
    }

    // ── 阶段 3：Capture 前再次刷 RigLogic（SaveAllData 会阻塞数十秒）────────────
    {
        const TMap<FName, float> Curves = EvaluateMetaHumanCurvesAtFrame(
            ActiveMetaHumanSequence.Keyframes, Frame);
        ApplyMetaHumanCurves(MetaHumanActor, Curves);
    }

    const bool bCaptureOK = CaptureDomeFrame(
        CaptureActor,
        ActiveMetaHumanSequence.CaptureComponentName,
        ActiveMetaHumanSequence.ExportPath,
        Frame);

    UE_LOG(LogTemp, Log, TEXT("[MetaHuman] Frame %d captured: %s"),
        Frame, bCaptureOK ? TEXT("OK") : TEXT("FAILED"));

    // 准备下一帧：重置为"尚未 apply"状态
    ActiveMetaHumanSequence.WaitFramesRemaining = -1;
    ++ActiveMetaHumanSequence.CurrentFrame;

    if (ActiveMetaHumanSequence.CurrentFrame >= ActiveMetaHumanSequence.TotalFrames)
    {
        // 序列完成。新策略下 AnimBP 全程未被暂停，这里的 resume 只是安全兜底
        // （万一 fallback 路径触发了 pause，确保恢复正常）。
        SetMetaHumanAnimPaused(MetaHumanActor, false);
        ActiveMetaHumanSequence.bAnimPaused = false;
        TArray<USkeletalMeshComponent*> DoneSkelComps;
        MetaHumanActor->GetComponents<USkeletalMeshComponent>(DoneSkelComps, true);
        for (USkeletalMeshComponent* SkelComp : DoneSkelComps)
        {
            if (SkelComp && IsMetaHumanFaceComponent(SkelComp))
            {
                SkelComp->MarkRenderDynamicDataDirty();
                SkelComp->MarkRenderStateDirty();
            }
        }
        ActiveMetaHumanSequence.bActive = false;
        UE_LOG(LogTemp, Warning, TEXT("[MetaHuman] Sequence DONE: %d frames -> %s"),
            ActiveMetaHumanSequence.TotalFrames, *ActiveMetaHumanSequence.ExportPath);
    }
}

void ARenderServer::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // 检查忙碌状态
    bool bAnyCameraBusy = false;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TArray<UFisheyeCameraComponent*> Cameras;
        It->GetComponents<UFisheyeCameraComponent>(Cameras);
        for (UFisheyeCameraComponent* FisheyeCamera : Cameras)
        {
            if (FisheyeCamera && FisheyeCamera->IsBusy())
            {
                bAnyCameraBusy = true;
                break;
            }
        }
        if (bAnyCameraBusy) break;
    }

    if (bAnyCameraBusy) 
    {
        // 如果有命令排队但相机忙碌，输出一条日志协助调试
        if (!IncomingCommands.IsEmpty())
        {
             static float LastBusyLogTime = 0;
             if (GetWorld()->GetTimeSeconds() - LastBusyLogTime > 2.0f)
             {
                 UE_LOG(LogTemp, Warning, TEXT("Commands are waiting, but camera is BUSY. Waiting for warm-up..."));
                 LastBusyLogTime = GetWorld()->GetTimeSeconds();
             }
        }
        return;
    }

    if (ActiveMetaHumanSequence.bActive)
    {
        TickMetaHumanSequence();
        return;
    }

    if (ActiveLevelSequenceCapture.bActive)
    {
        TickLevelSequenceCapture();
        return;
    }

    auto FindActorByNameOrLabel = [&](const FString& TargetName) -> AActor*
    {
        for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        {
            if (It->GetActorNameOrLabel().Equals(TargetName, ESearchCase::IgnoreCase) ||
                It->GetName().Contains(TargetName, ESearchCase::IgnoreCase))
            {
                return *It;
            }
        }
        return nullptr;
    };

    FCommandData Cmd;
    if (IncomingCommands.Dequeue(Cmd))
    {
        if (Cmd.Cmd == TEXT("set_metahuman_expression"))
        {
            AActor* TargetActor = FindActorByNameOrLabel(Cmd.ActorName);
            if (!TargetActor)
            {
                UE_LOG(LogTemp, Error, TEXT("[MetaHuman] set_metahuman_expression: Actor '%s' not found"), *Cmd.ActorName);
            }
            else
            {
                // preview 模式：如果 curves 为空则恢复动画，否则暂停 AnimBP 并应用曲线
                if (Cmd.MetaHumanCurves.Num() == 0)
                {
                    SetMetaHumanAnimPaused(TargetActor, false);
                    UE_LOG(LogTemp, Log, TEXT("[MetaHuman] Resumed AnimBP on '%s'"), *Cmd.ActorName);
                }
                else
                {
                    ApplyMetaHumanCurves(TargetActor, Cmd.MetaHumanCurves);
                    UE_LOG(LogTemp, Log, TEXT("[MetaHuman] Preview expression applied (RigLogic) on '%s'"), *Cmd.ActorName);
                }
            }
        }
        else if (Cmd.Cmd == TEXT("capture_metahuman_sequence"))
        {
            // 如果上一个序列还在跑，先确保动画恢复
            if (ActiveMetaHumanSequence.bActive && ActiveMetaHumanSequence.bAnimPaused)
            {
                AActor* OldMHActor = FindActorByNameOrLabel(ActiveMetaHumanSequence.MetaHumanActorName);
                if (OldMHActor) SetMetaHumanAnimPaused(OldMHActor, false);
            }

            ActiveMetaHumanSequence.bActive = true;
            ActiveMetaHumanSequence.MetaHumanActorName = Cmd.ActorName;
            ActiveMetaHumanSequence.CaptureActorName = Cmd.CaptureActorName.IsEmpty() ? TEXT("CameraRigActor") : Cmd.CaptureActorName;
            ActiveMetaHumanSequence.CaptureComponentName = Cmd.CaptureComponentName;
            ActiveMetaHumanSequence.ExportPath = Cmd.ExportPath;
            ActiveMetaHumanSequence.CurrentFrame = 0;
            ActiveMetaHumanSequence.WaitFramesRemaining = -1;  // -1 = 还没有对第 0 帧 Apply

            // 序列捕获时，必须暂停 AnimBP，防止引擎主循环在等待 GPU 帧期间将表情重置为默认值
            AActor* MHActor = FindActorByNameOrLabel(Cmd.ActorName);
            if (MHActor) SetMetaHumanAnimPaused(MHActor, false);
            ActiveMetaHumanSequence.bAnimPaused = false;

            ActiveMetaHumanSequence.Keyframes = Cmd.MetaHumanExpressionFrames;
            ActiveMetaHumanSequence.Keyframes.Sort([](const FMetaHumanExpressionFrame& A, const FMetaHumanExpressionFrame& B)
            {
                return A.Frame < B.Frame;
            });
            if (ActiveMetaHumanSequence.Keyframes.Num() == 0 && Cmd.MetaHumanCurves.Num() > 0)
            {
                FMetaHumanExpressionFrame SingleFrame;
                SingleFrame.Frame = 0;
                SingleFrame.Curves = Cmd.MetaHumanCurves;
                ActiveMetaHumanSequence.Keyframes.Add(MoveTemp(SingleFrame));
            }
            ActiveMetaHumanSequence.TotalFrames = Cmd.SequenceFrameCount > 0
                ? Cmd.SequenceFrameCount
                : (ActiveMetaHumanSequence.Keyframes.Num() > 0 ? ActiveMetaHumanSequence.Keyframes.Last().Frame + 1 : 1);
            UE_LOG(LogTemp, Warning, TEXT("[MetaHuman] Sequence started: actor=%s capture=%s frames=%d"),
                *ActiveMetaHumanSequence.MetaHumanActorName,
                *ActiveMetaHumanSequence.CaptureActorName,
                ActiveMetaHumanSequence.TotalFrames);
        }
        else if (Cmd.Cmd == TEXT("set_sequence_frame"))
        {
            const FString SequenceName = Cmd.SequenceActorName.IsEmpty() ? Cmd.ActorName : Cmd.SequenceActorName;
            const int32 TargetFrame = Cmd.FrameNumber >= 0 ? Cmd.FrameNumber : Cmd.StartFrame;
            const FString FaceActorName = (Cmd.ActorName == TEXT("World") && !Cmd.CaptureActorName.IsEmpty())
                ? Cmd.CaptureActorName
                : Cmd.ActorName;

            if (Cmd.bRenderAfterSequenceSeek && !Cmd.ExportPath.IsEmpty())
            {
                ActiveLevelSequenceCapture.bActive = true;
                ActiveLevelSequenceCapture.SequenceActorName = SequenceName;
                ActiveLevelSequenceCapture.MetaHumanActorName = FaceActorName;
                ActiveLevelSequenceCapture.CaptureActorName = Cmd.CaptureActorName.IsEmpty() ? TEXT("CameraRigActor") : Cmd.CaptureActorName;
                ActiveLevelSequenceCapture.CaptureComponentName = Cmd.CaptureComponentName;
                ActiveLevelSequenceCapture.ExportPath = Cmd.ExportPath;
                ActiveLevelSequenceCapture.CurrentFrame = TargetFrame;
                ActiveLevelSequenceCapture.EndFrame = TargetFrame;
                ActiveLevelSequenceCapture.FrameStep = 1;
                ActiveLevelSequenceCapture.EvaluationWaitFrames = FMath::Max(0, Cmd.EvaluationWaitFrames);
                ActiveLevelSequenceCapture.WaitFramesRemaining = -1;
                ActiveLevelSequenceCapture.bRenderAfterSeek = true;
                ActiveLevelSequenceCapture.FaceOverrideKeyframes = Cmd.MetaHumanExpressionFrames;
                UE_LOG(LogTemp, Warning, TEXT("[LevelSequence] Single-frame capture queued: sequence=%s frame=%d capture=%s"),
                    *SequenceName, TargetFrame, *ActiveLevelSequenceCapture.CaptureActorName);
            }
            else
            {
                SetLevelSequenceFrame(SequenceName, TargetFrame);
                if (Cmd.MetaHumanExpressionFrames.Num() > 0)
                {
                    if (AActor* MetaHumanActor = FindActorByNameOrLabel(FaceActorName))
                    {
                        PrepareMetaHumanForExternalFaceOverride(MetaHumanActor);
                        const TMap<FName, float> Curves = EvaluateMetaHumanCurvesAtFrame(Cmd.MetaHumanExpressionFrames, TargetFrame);
                        ApplyMetaHumanCurves(MetaHumanActor, Curves);
                    }
                }
            }
        }
        else if (Cmd.Cmd == TEXT("capture_level_sequence"))
        {
            const FString SequenceName = Cmd.SequenceActorName.IsEmpty() ? Cmd.ActorName : Cmd.SequenceActorName;
            const int32 StartFrame = Cmd.FrameNumber >= 0 ? Cmd.FrameNumber : Cmd.StartFrame;
            const int32 EndFrame = ResolveLevelSequenceEndFrame(SequenceName, StartFrame, Cmd.EndFrame, Cmd.SequenceFrameCount);
            const FString FaceActorName = (Cmd.ActorName == TEXT("World") && !Cmd.CaptureActorName.IsEmpty())
                ? Cmd.CaptureActorName
                : Cmd.ActorName;

            ActiveLevelSequenceCapture.bActive = true;
            ActiveLevelSequenceCapture.SequenceActorName = SequenceName;
            ActiveLevelSequenceCapture.MetaHumanActorName = FaceActorName;
            ActiveLevelSequenceCapture.CaptureActorName = Cmd.CaptureActorName.IsEmpty() ? TEXT("CameraRigActor") : Cmd.CaptureActorName;
            ActiveLevelSequenceCapture.CaptureComponentName = Cmd.CaptureComponentName;
            ActiveLevelSequenceCapture.ExportPath = Cmd.ExportPath;
            ActiveLevelSequenceCapture.CurrentFrame = StartFrame;
            ActiveLevelSequenceCapture.EndFrame = EndFrame;
            ActiveLevelSequenceCapture.FrameStep = FMath::Max(1, Cmd.FrameStep);
            ActiveLevelSequenceCapture.EvaluationWaitFrames = FMath::Max(0, Cmd.EvaluationWaitFrames);
            ActiveLevelSequenceCapture.WaitFramesRemaining = -1;
            ActiveLevelSequenceCapture.bRenderAfterSeek = Cmd.bRenderAfterSequenceSeek;
            ActiveLevelSequenceCapture.FaceOverrideKeyframes = Cmd.MetaHumanExpressionFrames;

            UE_LOG(LogTemp, Warning, TEXT("[LevelSequence] Capture started: sequence=%s capture=%s frames=%d..%d step=%d render=%s face_override=%d"),
                *ActiveLevelSequenceCapture.SequenceActorName,
                *ActiveLevelSequenceCapture.CaptureActorName,
                ActiveLevelSequenceCapture.CurrentFrame,
                ActiveLevelSequenceCapture.EndFrame,
                ActiveLevelSequenceCapture.FrameStep,
                ActiveLevelSequenceCapture.bRenderAfterSeek ? TEXT("true") : TEXT("false"),
                ActiveLevelSequenceCapture.FaceOverrideKeyframes.Num());
        }
        else if (Cmd.Cmd == TEXT("set_export_path"))
        {
            AActor* TargetActor = FindActorByNameOrLabel(Cmd.ActorName);
            if (TargetActor)
            {
                TArray<UFisheyeCameraComponent*> Cameras;
                TargetActor->GetComponents<UFisheyeCameraComponent>(Cameras);
                for (auto* Cam : Cameras) Cam->ExportBasePath = Cmd.ExportPath;
            }
        }
        else if (Cmd.Cmd == TEXT("save_frame"))
        {
            AActor* TargetActor = FindActorByNameOrLabel(Cmd.ActorName);
            if (TargetActor)
            {
                TArray<UFisheyeCameraComponent*> Cameras;
                TargetActor->GetComponents<UFisheyeCameraComponent>(Cameras);
                for (auto* Cam : Cameras)
                {
                    FString Path = Cmd.ExportPath.IsEmpty() ? Cam->ExportBasePath : Cmd.ExportPath;
                    int32 Num = Cmd.FrameNumber >= 0 ? Cmd.FrameNumber : Cam->FrameCounter;
                    Cam->SaveAllData(Path, Num);
                }
            }
        }
        else if (Cmd.Cmd == TEXT("set_pose"))
        {
            bool bFound = false;
            AActor* TargetActor = FindActorByNameOrLabel(Cmd.ActorName);
            if (TargetActor)
            {
                bFound = true;
                TargetActor->SetActorLocationAndRotation(Cmd.Position, Cmd.Rotation);

                TArray<UFisheyeCameraComponent*> Cameras;
                TargetActor->GetComponents<UFisheyeCameraComponent>(Cameras);

                for (auto* Cam : Cameras)
                {
                    Cam->CaptureFisheyeScene();

                    if (!Cmd.ExportPath.IsEmpty())
                    {
                        int32 Num = Cmd.FrameNumber >= 0 ? Cmd.FrameNumber : Cam->FrameCounter;
                        Cam->SaveAllData(Cmd.ExportPath, Num);
                    }
                }
            }
            if (!bFound) UE_LOG(LogTemp, Error, TEXT("[RenderServer] set_pose: Actor '%s' NOT FOUND in scene!"), *Cmd.ActorName);
        }
        else if (Cmd.Cmd == TEXT("configure_camera"))
        {
            AActor* TargetActor = FindActorByNameOrLabel(Cmd.ActorName);
            bool bFound = TargetActor != nullptr;
            if (bFound)
            {
                bFound = true;
                // AActor* It_Ref = TargetActor; // 替换掉原来的 *It
                FString CompName = Cmd.ComponentName.IsEmpty() ? TEXT("FisheyeCamera") : Cmd.ComponentName;
                UFisheyeCameraComponent* FisheyeCamera = nullptr;
                TArray<UFisheyeCameraComponent*> Cameras;
                TargetActor->GetComponents<UFisheyeCameraComponent>(Cameras);
                for (auto* Cam : Cameras) if (Cam->GetName() == CompName) { FisheyeCamera = Cam; break; }

// ... inside ParseCommandPayload or specific location ...

                if (!FisheyeCamera)
                {
                    if (Cmd.CameraModel.Contains(TEXT("dome"), ESearchCase::IgnoreCase))
                    {
                        FisheyeCamera = NewObject<UDomeLightStageCameraComponent>(TargetActor, UDomeLightStageCameraComponent::StaticClass(), *CompName);
                        UE_LOG(LogTemp, Log, TEXT("Created DomeLightStageCameraComponent"));
                    }
                    else if (Cmd.CameraModel.Contains(TEXT("erp"), ESearchCase::IgnoreCase))
                    {
                        FisheyeCamera = NewObject<UMultiViewERPCameraComponent>(TargetActor, UMultiViewERPCameraComponent::StaticClass(), *CompName);
                        UE_LOG(LogTemp, Log, TEXT("Created MultiViewERPCameraComponent"));
                    }
                    else if (Cmd.CameraModel.Contains(TEXT("multiview"), ESearchCase::IgnoreCase))
                    {
                        FisheyeCamera = NewObject<UMultiViewFisheyeCameraComponent>(TargetActor, UMultiViewFisheyeCameraComponent::StaticClass(), *CompName);
                        UE_LOG(LogTemp, Log, TEXT("Created MultiViewFisheyeCameraComponent"));
                    }
                    else
                    {
                        FisheyeCamera = NewObject<UFisheyeCameraComponent>(TargetActor, UFisheyeCameraComponent::StaticClass(), *CompName);
                    }
                    if (FisheyeCamera)
                    {
                        if (!Cmd.RGBMaterialPath.IsEmpty())
                        {
                            UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *Cmd.RGBMaterialPath);
                            if (Mat) FisheyeCamera->FisheyePostProcessMaterial = Mat;
                        }
                        if (!Cmd.DepthMaterialPath.IsEmpty())
                        {
                            UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *Cmd.DepthMaterialPath);
                            if (Mat) FisheyeCamera->FisheyeDepthPostProcessMaterial = Mat;
                        }
                        if (!Cmd.PositionMaterialPath.IsEmpty())
                        {
                            UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *Cmd.PositionMaterialPath);
                            if (Mat) FisheyeCamera->FisheyePositionPostProcessMaterial = Mat;
                        }
                    }
                    if (FisheyeCamera)
                    {
                        FisheyeCamera->RegisterComponent();
                        FisheyeCamera->AttachToComponent(TargetActor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
                    }
                }

                if (FisheyeCamera)
                {
                    if (!Cmd.RGBMaterialPath.IsEmpty())
                    {
                        UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *Cmd.RGBMaterialPath);
                        if (Mat) FisheyeCamera->FisheyeRGBFromCubemapMaterial = Mat;
                    }
                    if (!Cmd.DepthMaterialPath.IsEmpty())
                    {
                        UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *Cmd.DepthMaterialPath);
                        if (Mat) FisheyeCamera->FisheyeDepthFromCubemapMaterial = Mat;
                    }
                    FisheyeCamera->bUseCubemapForDepth = Cmd.bUseCubemap;
                    FisheyeCamera->CubemapResolution = Cmd.CubemapResolution;
                    FisheyeCamera->ExposureBias = Cmd.ExposureBias;
                    FisheyeCamera->bEnableToneCurve = Cmd.bEnableToneCurve;
                    FisheyeCamera->bUseHDR = Cmd.bUseHDR;
                    FisheyeCamera->WarmUpFrames = Cmd.WarmUpFrames;

                    FFisheyeParams Params;
                    Params.Resolution = Cmd.Resolution;
                    Params.CameraModel = Cmd.CameraModel;
                    Params.Intrinsics = Cmd.Intrinsics;
                    Params.DistortionCoeffs = Cmd.DistortionCoeffs;
                    Params.ExposureBias = Cmd.ExposureBias;
                    Params.bEnableToneCurve = Cmd.bEnableToneCurve;
                    Params.bUseHDR = Cmd.bUseHDR;
                    Params.WarmUpFrames = Cmd.WarmUpFrames;

                    if (Params.Intrinsics.Num() >= 4)
                    {
                        Params.Fx = Params.Intrinsics[0]; Params.Fy = Params.Intrinsics[1];
                        Params.Cx = Params.Intrinsics[2]; Params.Cy = Params.Intrinsics[3];
                    }
                    Params.SupersamplingMultiplier = Cmd.SupersamplingMultiplier;
                    Params.RGBMaterialPath = Cmd.RGBMaterialPath;
                    Params.DepthMaterialPath = Cmd.DepthMaterialPath;
                    Params.PositionMaterialPath = Cmd.PositionMaterialPath;
                    // Copy MultiView params
                    Params.AngleStep = Cmd.AngleStep;
                    Params.HorizontalFOV = Cmd.HorizontalFOV;
                    Params.VerticalFOV = Cmd.VerticalFOV;
                    Params.RenderMode = Cmd.RenderMode;
                    Params.NumParallelProbes = Cmd.NumParallelProbes;

                    FisheyeCamera->ConfigureCamera(Params);

                    // ── Dome LightStage 额外配置 ──
                    UDomeLightStageCameraComponent* DomeCam = Cast<UDomeLightStageCameraComponent>(FisheyeCamera);
                    if (DomeCam)
                    {
                        FDomeCameraConfig DomeCfg;
                        DomeCfg.DomeRadius = Cmd.DomeRadius;
                        DomeCfg.NumCameras = Cmd.DomeNumCameras;
                        DomeCfg.CameraFOV = Cmd.DomeCameraFOV;
                        DomeCfg.Resolution = Cmd.Resolution;
                        DomeCfg.ExposureBias = Cmd.ExposureBias;
                        DomeCfg.bEnableToneCurve = Cmd.bEnableToneCurve;
                        DomeCfg.WarmUpFrames = Cmd.WarmUpFrames;
                        DomeCfg.RenderBatchSize = Cmd.DomeRenderBatchSize;
                        DomeCfg.EnabledPasses = Cmd.DomeEnabledPasses;
                        DomeCfg.bUpperHemisphereOnly = Cmd.bDomeUpperHemisphereOnly;
                        DomeCfg.DepthMaterialPath = Cmd.DepthMaterialPath;
                        DomeCfg.NormalMaterialPath = Cmd.NormalMaterialPath;
                        DomeCfg.PositionMaterialPath = Cmd.PositionMaterialPath;
                        DomeCfg.SemanticMaterialPath = Cmd.SemanticMaterialPath;

                        // 布局模式
                        if (Cmd.DomeLayout.Contains(TEXT("ring"), ESearchCase::IgnoreCase))
                            DomeCfg.Layout = EDomeCameraLayout::Rings;
                        else if (Cmd.DomeLayout.Contains(TEXT("manual"), ESearchCase::IgnoreCase))
                            DomeCfg.Layout = EDomeCameraLayout::Manual;
                        else
                            DomeCfg.Layout = EDomeCameraLayout::FibonacciSphere;

                        // Ring 模式数据
                        if (Cmd.DomeCamerasPerRing.Num() > 0)
                            DomeCfg.CamerasPerRing = Cmd.DomeCamerasPerRing;
                        if (Cmd.DomeRingElevations.Num() > 0)
                            DomeCfg.RingElevations = Cmd.DomeRingElevations;

                        // Manual 模式：传入手动相机列表
                        if (Cmd.DomeManualCameras.Num() > 0)
                            DomeCfg.ManualCameras = Cmd.DomeManualCameras;

                        DomeCam->ConfigureDome(DomeCfg);
                        UE_LOG(LogTemp, Warning, TEXT("[RenderServer] Dome configured: %d cameras, layout=%s"),
                            DomeCfg.NumCameras, *Cmd.DomeLayout);
                    }
                }
            }
            if (!bFound) UE_LOG(LogTemp, Error, TEXT("[RenderServer] configure_camera: Actor '%s' NOT FOUND in scene!"), *Cmd.ActorName);
        }
        else if (Cmd.Cmd == TEXT("clear_cameras"))
        {
            AActor* TargetActor = FindActorByNameOrLabel(Cmd.ActorName);

            if (TargetActor)
            {
                TArray<UFisheyeCameraComponent*> Cameras;
                TargetActor->GetComponents<UFisheyeCameraComponent>(Cameras);
                for (auto* Cam : Cameras) Cam->DestroyComponent();
                TArray<UCameraComponent*> DefaultCameras;
                TargetActor->GetComponents<UCameraComponent>(DefaultCameras);
                for (auto* Cam : DefaultCameras) Cam->DestroyComponent();
            }
        }
        else if (Cmd.Cmd == TEXT("create_lights"))
        {
            FLightManager::ProcessLights(GetWorld(), Cmd.Lights);
        }
    }

    FDataExporter::ProcessPendingReadbacks();
}

void ARenderServer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bShouldRunClientReceiveThread = false;
    if (ClientReceiveThread) { ClientReceiveThread->Kill(true); delete ClientReceiveThread; ClientReceiveThread = nullptr; }
    if (ClientSocket) { ClientSocket->Close(); ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket); ClientSocket = nullptr; }
    if (TcpListener) { TcpListener->Stop(); delete TcpListener; TcpListener = nullptr; }
    Super::EndPlay(EndPlayReason);
}
