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
#include "FisheyeCameraComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UObject/Package.h"

#include "DomeLightStageCameraComponent.generated.h"

class USkeletalMeshComponent;
class UMeshComponent;
class FJsonObject;
class UMaterialInstanceDynamic;

USTRUCT()
struct FSemanticMeshEntry
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<FColor> SemanticColors; // LOD0 顶点数，逐顶点语义色

    UPROPERTY()
    UMaterialInstanceDynamic* SemanticMID = nullptr;

    UPROPERTY()
    int32 NumMaterials = 0;
};

/**
 * Dome LightStage 穹顶相机工装组件
 *
 * Semantic 通道采用：
 *   运行时读取骨骼蒙皮权重 → 计算每顶点语义色
 *   → SetVertexColorOverride_GameThread 直写顶点色缓冲
 *   → 材质用 VertexColor 节点输出（完全不依赖 UV）
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class RENDERCOREEXT_API UDomeLightStageCameraComponent : public UFisheyeCameraComponent
{
    GENERATED_BODY()

public:
    UDomeLightStageCameraComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    FDomeCameraConfig DomeConfig;

    UFUNCTION(BlueprintCallable, Category = "Dome")
    void ConfigureDome(const FDomeCameraConfig& Config);

    UFUNCTION(BlueprintPure, Category = "Dome")
    const TArray<FDomeCameraView>& GetDomeCameras() const { return DomeCameras; }

    UFUNCTION(BlueprintPure, Category = "Dome")
    int32 GetDomeCameraCount() const { return DomeCameras.Num(); }

    /**
     * 重新扫描场景，重建每个 MetaHuman SkelComp 的语义顶点色缓冲。
     * 换装或新增角色后调用。
     */
    UFUNCTION(BlueprintCallable, Category = "Dome|Semantic")
    void RebuildSemanticTextureCache();

    virtual void CaptureFisheyeScene() override;
    virtual bool SaveAllData(const FString& BasePath, int32 FrameNumber = -1) override;

protected:
    virtual void BeginPlay() override;

private:
    // ── 相机列表 ──────────────────────────────────────────────────────────────
    TArray<FDomeCameraView> DomeCameras;

    // ── 探针池 ────────────────────────────────────────────────────────────────
    UPROPERTY() TArray<USceneCaptureComponent2D*> ProbeCaptures;
    UPROPERTY() TArray<UTextureRenderTarget2D*>   ProbeRGBRTs;
    UPROPERTY() TArray<UTextureRenderTarget2D*>   ProbeAuxRTs;

    // ── Semantic：顶点色覆盖缓存 ──────────────────────────────────────────────
    //
    // 方案：运行时对每个 SkelComp 调用
    //   SetVertexColorOverride_GameThread(LODIndex, Colors)
    // 材质里仅需 VertexColor → Emissive，无需危险的贴图或 UV。
    //
    // 每个 SkelComp 缓存：
    //   - SemanticColors : 按顶点索引排列的语义色（与 LOD0 顶点数对齐）
    //   - SemanticMID    : Unlit VertexColor 材质实例
    UPROPERTY(Transient)
    TMap<USkeletalMeshComponent*, FSemanticMeshEntry> SemanticCache;

    /**
     * 用于 Semantic Pass 的基础材质路径。
     * 必须是 Unlit 材质，直接把 VertexColor 连到 Emissive。
     * 留空时编辑器模式下自动创建。
     */
    UPROPERTY(EditAnywhere, Category = "Dome|Semantic")
    FString SemanticVertexColorMaterialPath;

    // ── Semantic：槽级材质（兜底） ────────────────────────────────────────────
    UPROPERTY(Transient)
    TMap<FName, UMaterialInterface*> SemanticRegionMaterials;

    // ── 内部构建 ──────────────────────────────────────────────────────────────
    void BuildDomeCameras();
    void BuildFibonacciSphere();
    void BuildRingLayout();
    void RebuildProbePool();
    void DestroyProbePool();

    // ── Semantic 主路径（顶点色覆盖）────────────────────────────────────────
    void BuildSemanticCacheForScene();

    /**
     * 计算 SkelComp LOD0 每个顶点的语义色，存入 OutColors。
     * 返回 false 表示该 Mesh 无蒙皮数据，跳过。
     */
    bool BuildSemanticColorsForMesh(
        USkeletalMeshComponent* SkelComp,
        TArray<FColor>& OutColors) const;

    /**
     * 单顶点：按蒙皮权重加权混合各骨骼语义色。
     */
    FLinearColor ComputeVertexSemanticColor(
        int32 VertIdx,
        const FSkinWeightVertexBuffer* SkinWeights,
        const FReferenceSkeleton& RefSkel,
        int32 NumInfluences) const;

    /**
     * 骨骼名 → 语义色。精确匹配优先，前缀/包含匹配次之。
     */
    FLinearColor ResolveBoneSemanticColor(const FString& BoneName) const;

    /**
     * 创建 Unlit VertexColor MID（或加载用户指定材质）。
     */
    UMaterialInstanceDynamic* CreateSemanticVertexColorMID(
        USkeletalMeshComponent* SkelComp) const;

    /**
     * Semantic Pass：临时覆盖顶点色 + 材质，捕获，还原。
     */
    void RenderSemanticWithVertexColor(
        int32 ProbeIdx,
        UTextureRenderTarget2D* TargetRT,
        const FString& OutputPath);

    // ── Semantic 通用入口 ────────────────────────────────────────────────────
    void RenderAndExportSemantic(
        int32 ProbeIdx,
        UTextureRenderTarget2D* TargetRT,
        const FString& OutputPath);

    // ── Semantic 兜底（槽级材质）────────────────────────────────────────────
    void EnsureSemanticRegionMaterials();
    UMaterialInterface* CreateSemanticColorMaterial(
        const FName& RegionName, const FLinearColor& Color);
    FName GetSemanticRegionForMeshSlot(
        UMeshComponent* MeshComp, int32 MaterialIndex) const;

    // ── 骨骼工具 ─────────────────────────────────────────────────────────────
    bool IsSemanticCandidateMesh(USkeletalMeshComponent* SkelComp) const;
    bool HasBoneName(UMeshComponent* MeshComp, const FName& BoneName) const;
    bool HasBoneNameContaining(
        UMeshComponent* MeshComp, const FString& BoneNamePart) const;

    // ── AOV / 内外参 ─────────────────────────────────────────────────────────
    void RenderAndExportAOV(
        int32 ProbeIdx, UMaterialInterface* Material,
        UTextureRenderTarget2D* TargetRT, const FString& OutputPath,
        bool bUseHDRFormat, bool bDisableAA = true);

    TSharedPtr<FJsonObject> BuildIntrinsicJson(float FOVDeg, FIntPoint Res) const;
    TSharedPtr<FJsonObject> BuildExtrinsicJson(
        const FVector& WorldPos, const FQuat& WorldRot) const;
};