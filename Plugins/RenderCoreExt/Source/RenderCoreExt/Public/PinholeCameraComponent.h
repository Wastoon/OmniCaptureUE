#pragma once

#include "CoreMinimal.h"
#include "MultiViewFisheyeCameraComponent.h"
#include "PinholeCameraComponent.generated.h"

/**
 * Pinhole Camera Component
 * Designed for single-view high-speed rendering.
 * Supports parallelizing RGB, Depth, and Position passes across multiple probes.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class RENDERCOREEXT_API UPinholeCameraComponent : public UMultiViewFisheyeCameraComponent
{
    GENERATED_BODY()

public:
    UPinholeCameraComponent();

    // Override to only generate a single view based on current component pose
    virtual void RebuildMultiViewCaptures() override;

    // Optimized SaveAllData for single-pose parallel passes
    virtual bool SaveAllData(const FString& BasePath, int32 FrameNumber = -1) override;
};
