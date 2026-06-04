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
#include "RenderGraphResources.h"
#include "RHIGPUReadback.h"

class FSceneView;

struct FReadbackData
{
    TUniquePtr<FRHIGPUTextureReadback> ReadbackPtr;
    FIntPoint Size;
    FString BufferName;

    FReadbackData(FIntPoint InSize, FString InBufferName)
        : Size(InSize), BufferName(InBufferName)
    {
        ReadbackPtr = MakeUnique<FRHIGPUTextureReadback>(*InBufferName);
    }

    bool IsReady() const { return ReadbackPtr->IsReady(); }

    void* Lock(int32& OutRowPitchInPixels) const
    {
        return ReadbackPtr->Lock(OutRowPitchInPixels);
    }

    void Unlock() const { ReadbackPtr->Unlock(); }
};

class RENDERCOREEXT_API FDataExporter
{
public:
    static void AddDataExportPass(FRDGBuilder& GraphBuilder, FRDGTextureRef SceneColor, FRDGTextureRef SceneDepth, const FSceneView& View);
    static void ProcessPendingReadbacks();

private:
    static void OnReadbackCompleted(void* Data, int32 RowPitch, FIntPoint Size, FString BufferName);
};

extern TArray<TSharedPtr<FReadbackData>> PendingReadbacks;
extern FCriticalSection PendingReadbacksCS;
