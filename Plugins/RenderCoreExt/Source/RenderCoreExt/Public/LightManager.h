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
#include "Dom/JsonObject.h"
#include "Engine/RectLight.h"
#include "Components/RectLightComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * 结构体：存储单个灯光的数据
 */
struct FLightData
{
    FString Name;
    FString Type; // "POINT", "SPOT", "AREA"
    bool bEnabled = true;
    FVector Location;
    FRotator Rotation;
    float Intensity;
    FLinearColor Color;
    
    // RectLight 特有
    float SourceWidth = 100.0f;
    float SourceHeight = 100.0f;
    float AttenuationRadius = 1000.0f;
    float Spread = 180.0f;
    bool bUseShadow = true;
};

/**
 * 类：负责解耦灯光配置的解析、转换与生成
 */
class RENDERCOREEXT_API FLightManager
{
public:
    /**
     * 解析 JSON 数组或对象中的灯光配置
     */
    static void ParseLights(const TSharedPtr<FJsonObject>& JsonObject, const TArray<TSharedPtr<FJsonValue>>* JsonArray, TArray<FLightData>& OutLights);

    /**
     * 在世界中执行灯光的生成或更新
     */
    static void ProcessLights(UWorld* World, const TArray<FLightData>& Lights);

private:
    /**
     * 辅助解析单个灯光 JSON 对象
     */
    static bool ParseSingleLight(const TSharedPtr<FJsonObject>& LightObj, FLightData& OutData);
};
