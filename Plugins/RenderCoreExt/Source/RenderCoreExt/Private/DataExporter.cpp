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

#include "DataExporter.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

TArray<TSharedPtr<FReadbackData>> PendingReadbacks;
FCriticalSection PendingReadbacksCS;

void FDataExporter::AddDataExportPass(FRDGBuilder& GraphBuilder, FRDGTextureRef SceneColor, FRDGTextureRef SceneDepth, const FSceneView& View)
{
    if (!SceneColor) return;

    FIntPoint Size = SceneColor->Desc.Extent;
    TSharedPtr<FReadbackData> ColorReadback = MakeShared<FReadbackData>(Size, TEXT("SceneColor"));

    AddReadbackTexturePass(GraphBuilder, RDG_EVENT_NAME("ExportSceneColor"), SceneColor,
        [ColorReadback, SceneColor](FRHICommandListImmediate& RHICmdList)
        {
            ColorReadback->ReadbackPtr->EnqueueCopy(RHICmdList, SceneColor->GetRHI(), FResolveRect(0, 0, ColorReadback->Size.X, ColorReadback->Size.Y));
        });

    {
        FScopeLock Lock(&PendingReadbacksCS);
        PendingReadbacks.Add(ColorReadback);
    }
}

void FDataExporter::ProcessPendingReadbacks()
{
    FScopeLock Lock(&PendingReadbacksCS);
    for (int32 i = PendingReadbacks.Num() - 1; i >= 0; --i)
    {
        TSharedPtr<FReadbackData> Readback = PendingReadbacks[i];
        if (Readback->IsReady())
        {
            int32 RowPitch = 0;
            void* DataPtr = Readback->Lock(RowPitch);
            if (DataPtr)
            {
                OnReadbackCompleted(DataPtr, RowPitch, Readback->Size, Readback->BufferName);
                Readback->Unlock();
            }
            PendingReadbacks.RemoveAt(i);
        }
    }
}

void FDataExporter::OnReadbackCompleted(void* Data, int32 RowPitch, FIntPoint Size, FString BufferName)
{
    UE_LOG(LogTemp, Log, TEXT("Successfully read back %s: %dx%d, RowPitch: %d"), *BufferName, Size.X, Size.Y, RowPitch);
}