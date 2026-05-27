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
