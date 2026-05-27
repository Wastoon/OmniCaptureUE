#pragma once

#include "CoreMinimal.h"
#include "FisheyeCameraComponent.h"
#include "MultiViewERPCameraComponent.generated.h"

/**
 * 多视角 ERP (全景) 相机组件
 * 专门针对 360x180 全景图采样设计的组件。
 * 使用优化的球面网格采样（极点消减），在保证全景覆盖的前提下，显存和耗时比常规网格降低 ~40%。
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class RENDERCOREEXT_API UMultiViewERPCameraComponent : public UFisheyeCameraComponent
{
    GENERATED_BODY()

public:
    UMultiViewERPCameraComponent();

    virtual void ConfigureCamera(const FFisheyeParams& Params) override;
    virtual void CaptureFisheyeScene() override;
    virtual bool SaveAllData(const FString& BasePath, int32 FrameNumber = -1) override;

protected:
    virtual void BeginPlay() override;

    UPROPERTY()
    USceneCaptureComponent2D* SharedCaptureComp;

    UPROPERTY()
    UTextureRenderTarget2D* SharedDepthRT;

    UPROPERTY()
    TArray<FMultiViewProbe> ProbePool;

private:
    void RebuildSphericalSampling();
    void SetupProbe(FMultiViewProbe& OutProbe, int32 Index);

    TArray<FMultiViewData> ViewsMetadata;
};
