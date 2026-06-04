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
