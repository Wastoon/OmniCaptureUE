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

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// #include "Networking.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/TcpListener.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/Queue.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h" 
#include "LightManager.h"
#include "FisheyeCameraComponent.h"
#include "DomeLightStageCameraComponent.h"
#include "RenderServer.generated.h"

// 协议头
#pragma pack(push, 1)
struct FProtocolHeader
{
    uint32 Magic;      // 0x55453552 (UE5R)
    uint16 Version;    // 0x0001
    uint16 PayloadType; // 0=Command, 1=Data, 2=Status
    uint32 PayloadLength; // Length of the following payload
};
#pragma pack(pop)

struct FMetaHumanExpressionFrame
{
    int32 Frame = 0;
    TMap<FName, float> Curves;
};

struct FMetaHumanSequenceState
{
    bool bActive = false;
    FString MetaHumanActorName;
    FString CaptureActorName;
    FString CaptureComponentName;
    FString ExportPath;
    int32 CurrentFrame = 0;
    int32 TotalFrames = 0;
    // 每帧拍摄的三阶段状态计数：
    //  -1 = 还未对本帧 Apply 曲线（需先执行 apply）
    //  >0 = 已 apply，倒计时等待 GPU 稳定（等 N 帧）
    //   0 = GPU 已稳定，可以执行 Capture
    int32 WaitFramesRemaining = -1;
    bool bAnimPaused = false;   // 记录是否已经暂停了 MetaHuman 的 AnimBP
    TArray<FMetaHumanExpressionFrame> Keyframes;
};

struct FLevelSequenceCaptureState
{
    bool bActive = false;
    FString SequenceActorName;
    FString MetaHumanActorName;
    FString CaptureActorName;
    FString CaptureComponentName;
    FString ExportPath;
    int32 CurrentFrame = 0;
    int32 EndFrame = 0;
    int32 FrameStep = 1;
    int32 EvaluationWaitFrames = 2;
    int32 WaitFramesRemaining = -1;
    bool bRenderAfterSeek = true;
    TArray<FMetaHumanExpressionFrame> FaceOverrideKeyframes;
};

/**
 * 相机模型材质映射
 */
USTRUCT(BlueprintType)
struct FCameraModelMaterial
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RenderServer")
    UMaterialInterface* PostProcessMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RenderServer")
    UMaterialInterface* DepthPostProcessMaterial;
};

// 命令结构体
USTRUCT(BlueprintType)
struct FCommandData
{
    GENERATED_BODY()
    FString Cmd;
    FString ActorName;
    FVector Position;
    FQuat Rotation;
    FString ExportPath;  // 导出路径
    int32 FrameNumber = -1;  // 帧号

    // 相机配置参数
    FIntPoint Resolution = FIntPoint(1280, 720);
    FString CameraModel;
    TArray<float> Intrinsics;
    TArray<float> DistortionCoeffs;
    FString ComponentName;      // 组件名称
    FString RGBMaterialPath;    // RGB材质路径
    FString DepthMaterialPath;  // 深度材质路径
    FString PositionMaterialPath; // 位置材质路径
    int32 CubemapResolution = 512; // Cubemap 分辨率
    bool bUseCubemap = false;      // 是否使用 Cubemap 模式 (鱼眼模式)
    float ExposureBias = 0.0f;     // 曝光补偿
    bool bEnableToneCurve = true;  // 是否启用色调映射
    bool bUseHDR = true;           // 是否使用 HDR 管线
    int32 WarmUpFrames = 10;       // 预热帧数
    float SupersamplingMultiplier = 1.0f; // 超级采样倍率

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command", meta = (DisplayName = "angle_step"))
    float AngleStep = 30.0f; // Default 30

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command", meta = (DisplayName = "horizontal_fov"))
    float HorizontalFOV = 180.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command", meta = (DisplayName = "vertical_fov"))
    float VerticalFOV = 180.0f; // Default 180

    EMultiViewRenderMode RenderMode = EMultiViewRenderMode::Serial;
    int32 NumParallelProbes = 4;

    // 灯光数据
    TArray<FLightData> Lights;

    TMap<FName, float> MetaHumanCurves;
    TArray<FMetaHumanExpressionFrame> MetaHumanExpressionFrames;
    FString CaptureActorName;
    FString CaptureComponentName;
    int32 SequenceFrameCount = 0;
    FString SequenceActorName;
    int32 StartFrame = 0;
    int32 EndFrame = -1;
    int32 FrameStep = 1;
    int32 EvaluationWaitFrames = 2;
    bool bRenderAfterSequenceSeek = true;

    // ── Dome LightStage 参数 ─────────────────────────────────────────────
    float DomeRadius = 200.0f;                     // 穹顶半径 (cm)
    int32 DomeNumCameras = 32;                     // 相机数量
    FString DomeLayout = TEXT("fibonacci_sphere"); // fibonacci_sphere | rings | manual
    float DomeCameraFOV = 50.0f;                   // 每个相机 FOV
    int32 DomeRenderBatchSize = 4;                 // 并行批次大小
    uint8 DomeEnabledPasses = 0x1F;                // 通道位标志 (RGB|Depth|Normal|Position|Semantic)
    bool bDomeUpperHemisphereOnly = true;          // 是否仅上半球
    TArray<int32> DomeCamerasPerRing;              // Ring 模式：每环相机数
    TArray<float> DomeRingElevations;              // Ring 模式：每环仰角(度)
    FString NormalMaterialPath;                    // Normal 通道后处理材质路径
    FString SemanticMaterialPath;                  // Semantic 通道后处理材质路径
    TArray<FDomeCameraView> DomeManualCameras;     // Manual 模式：从 Python JSON 解析的相机列表
};

/**
 * TCP Socket Server Actor，用于接收 Python 指令
 */
UCLASS()
class RENDERCOREEXT_API ARenderServer : public AActor
{
    GENERATED_BODY()

public:
    ARenderServer();

    // 监听端口
    UPROPERTY(EditAnywhere, Category = "RenderServer")
    int32 ListenPort = 9998;

    // 相机模型到材质的映射
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RenderServer")
    TMap<FString, FCameraModelMaterial> CameraModelMap;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaTime) override;

private:
    // TCP 监听
    FTcpListener* TcpListener = nullptr;
    FSocket* ClientSocket = nullptr;
    FRunnableThread* ClientReceiveThread = nullptr;
    bool bShouldRunClientReceiveThread = false;

    // 连接回调
    bool OnConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& InEndpoint);

    // 线程安全队列，用于存储接收到的命令
    TQueue<FCommandData> IncomingCommands;

    FMetaHumanSequenceState ActiveMetaHumanSequence;
    FLevelSequenceCaptureState ActiveLevelSequenceCapture;

    AActor* FindActorByNameOrLabel(const FString& TargetName) const;
    class ALevelSequenceActor* FindLevelSequenceActorByNameOrLabel(const FString& TargetName) const;
    bool ApplyMetaHumanCurves(AActor* TargetActor, const TMap<FName, float>& Curves) const;
    /** 暂停 / 恢复 MetaHuman 所有 SkeletalMeshComponent 的动画评估 */
    void SetMetaHumanAnimPaused(AActor* TargetActor, bool bPause) const;
    TMap<FName, float> EvaluateMetaHumanCurvesAtFrame(
        const TArray<FMetaHumanExpressionFrame>& Keyframes, int32 Frame) const;
    bool CaptureDomeFrame(
        AActor* CaptureActor, const FString& ComponentName,
        const FString& ExportPath, int32 FrameNumber);
    bool SetLevelSequenceFrame(const FString& SequenceActorName, int32 FrameNumber) const;
    int32 ResolveLevelSequenceEndFrame(const FString& SequenceActorName, int32 StartFrame, int32 RequestedEndFrame, int32 FrameCount) const;
    bool CaptureLevelSequenceFrame(const FLevelSequenceCaptureState& State, int32 FrameNumber);
    void TickMetaHumanSequence();
    void TickLevelSequenceCapture();
};


// 客户端接收线程
class FClientReceiveRunnable : public FRunnable
{
public:
    FClientReceiveRunnable(FSocket* InClientSocket, TQueue<FCommandData>& InCommandQueue, bool& InShouldRunFlag);
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    virtual void Exit() override;

private:
    FSocket* ClientSocket;
    TQueue<FCommandData>& CommandQueue;
    bool& bShouldRun;

    // 解析协议
    bool ParseCommandPayload(const TArray<uint8>& Data, int32 PayloadOffset, int32 PayloadLength, FCommandData& OutCommand);
};
