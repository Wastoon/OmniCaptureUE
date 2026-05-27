#pragma once

#include "CoreMinimal.h"
#include "FisheyeCameraComponent.h"
#include "MultiViewFisheyeCameraComponent.generated.h"

/**
 * 多视角鱼眼相机组件
 * 通过在一个球面上移动并连续捕获多个透视视图来模拟鱼眼/全景数据，
 * 这种方式可以完美支持 Lumen、RayTracing 等高级渲染特性。
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class RENDERCOREEXT_API UMultiViewFisheyeCameraComponent : public UFisheyeCameraComponent
{
    GENERATED_BODY()

public:
    UMultiViewFisheyeCameraComponent();

    virtual void ConfigureCamera(const FFisheyeParams& Params) override;
    virtual void CaptureFisheyeScene() override;
    virtual bool SaveAllData(const FString& BasePath, int32 FrameNumber = -1) override;

protected:
    virtual void RebuildMultiViewCaptures();
    void SetupProbe(FMultiViewProbe& OutProbe, int32 Index);
    virtual void BeginPlay() override;

    UPROPERTY()
    UTextureRenderTarget2D* SharedDepthRT;

    UPROPERTY()
    UTextureRenderTarget2D* SharedRGBRT;

    UPROPERTY()
    USceneCaptureComponent2D* SharedCaptureComp;

    UPROPERTY()
    TArray<FMultiViewProbe> ProbePool;

    TArray<FMultiViewData> ViewsMetadata;
};
