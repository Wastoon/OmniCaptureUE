#include "LightManager.h"
#include "EngineUtils.h"
#include "Components/RectLightComponent.h"
#include "Engine/RectLight.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void FLightManager::ParseLights(const TSharedPtr<FJsonObject>& JsonObject, const TArray<TSharedPtr<FJsonValue>>* JsonArray, TArray<FLightData>& OutLights)
{
    if (JsonObject.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* LightsVal = nullptr;
        if (JsonObject->TryGetArrayField(TEXT("lights"), LightsVal))
        {
            for (auto& Val : *LightsVal)
            {
                FLightData LData;
                if (ParseSingleLight(Val->AsObject(), LData))
                {
                    OutLights.Add(LData);
                }
            }
        }
    }
    else if (JsonArray)
    {
        for (auto& Val : *JsonArray)
        {
            FLightData LData;
            if (ParseSingleLight(Val->AsObject(), LData))
            {
                OutLights.Add(LData);
            }
        }
    }
}

// bool FLightManager::ParseSingleLight(const TSharedPtr<FJsonObject>& LightObj, FLightData& OutData)
// {
//     if (!LightObj.IsValid()) return false;

//     OutData.Name = LightObj->GetStringField(TEXT("name"));
//     OutData.Type = LightObj->GetStringField(TEXT("type"));
//     OutData.bEnabled = LightObj->HasField(TEXT("enabled")) ? LightObj->GetBoolField(TEXT("enabled")) : true;
//     OutData.bUseShadow = LightObj->HasField(TEXT("use_shadow")) ? LightObj->GetBoolField(TEXT("use_shadow")) : true;

//     // --- 坐标系统转换 (Blender -> UE) ---
//     const TArray<TSharedPtr<FJsonValue>>* LPos = nullptr;
//     if (LightObj->TryGetArrayField(TEXT("location"), LPos) && LPos->Num() == 3)
//     {
//         float BX = (*LPos)[0]->AsNumber();
//         float BY = (*LPos)[1]->AsNumber();
//         float BZ = (*LPos)[2]->AsNumber();
//         // UE: X=Forward(BY), Y=Right(BX), Z=Up(BZ). 单位 m -> cm.
//         OutData.Location = FVector(BY * 100.0f, BX * 100.0f, BZ * 100.0f);
//     }
//     // --- 坐标系统转换 (Blender -> UE) ---
//     const TArray<TSharedPtr<FJsonValue>>* LRot = nullptr;
//     if (LightObj->TryGetArrayField(TEXT("rotation_euler"), LRot) && LRot->Num() == 3)
//     {
//         // Blender Euler (rad)
//         const float RX = (*LRot)[0]->AsNumber(); // X
//         const float RY = (*LRot)[1]->AsNumber(); // Y
//         const float RZ = (*LRot)[2]->AsNumber(); // Z

//         // --- 修正后的坐标系转换 (Basis Vector Mapping) ---
//         // 1. 从 Blender Euler 构建旋转四元数 (Blender: ZYX order)
//         // Blender Axes: X=Right, Y=Forward, Z=Up
//         const FQuat Qx(FVector(1, 0, 0), RX);
//         const FQuat Qy(FVector(0, 1, 0), RY);
//         const FQuat Qz(FVector(0, 0, 1), RZ);
//         FQuat BlenderQuat = Qz * Qy * Qx;

//         // 2. 构建 Blender 旋转矩阵
//         FMatrix BMat = FRotationMatrix::Make(BlenderQuat);
        
//         // 3. 提取 Blender 坐标系下的基向量 (Right, Forward, Up)
//         FVector B_Right = BMat.GetUnitAxis(EAxis::X);
//         FVector B_Fwd   = BMat.GetUnitAxis(EAxis::Y);
//         FVector B_Up    = BMat.GetUnitAxis(EAxis::Z);
        
//         // 4. 映射到 UE 坐标系 (Left-Handed Z-up)
//         // 变换规则 C: (x, y, z) -> (y, x, z)
//         // 即: UE.X = B.Y, UE.Y = B.X, UE.Z = B.Z
        
//         // 将基向量变换到 UE 空间
//         FVector UE_Fwd_Vec   = FVector(B_Fwd.Y, B_Fwd.X, B_Fwd.Z);   // C(B_Fwd)
//         FVector UE_Right_Vec = FVector(B_Right.Y, B_Right.X, B_Right.Z); // C(B_Right)
//         FVector UE_Up_Vec    = FVector(B_Up.Y, B_Up.X, B_Up.Z);      // C(B_Up)
        
//         // 5. 构建 UE 旋转矩阵
//         // UE Matrix Columns: [Forward | Right | Up] <=> [X | Y | Z]
//         FMatrix UEMat = FMatrix(UE_Fwd_Vec, UE_Right_Vec, UE_Up_Vec, FVector::ZeroVector);
//         FQuat UEQuat = UEMat.ToQuat();
        
//         // 6. 修正光照朝向
//         // Blender Light 默认朝向 -Z，UE RectLight 默认朝向 +X
//         // 需要将 +X 旋转到 -Z，即 Pitch -90 度
//         FQuat LightOffset = FRotator(-90.0f, 0.0f, 0.0f).Quaternion();
//         OutData.Rotation = (UEQuat * LightOffset).Rotator();
//     }


//     OutData.Intensity = LightObj->GetNumberField(TEXT("energy"));
    
//     const TArray<TSharedPtr<FJsonValue>>* LCol = nullptr;
//     if (LightObj->TryGetArrayField(TEXT("color"), LCol) && LCol->Num() == 3)
//     {
//         OutData.Color = FLinearColor((*LCol)[0]->AsNumber(), (*LCol)[1]->AsNumber(), (*LCol)[2]->AsNumber());
//     }

//     OutData.SourceWidth = LightObj->GetNumberField(TEXT("area_size")) * 100.0f;
//     OutData.SourceHeight = LightObj->GetNumberField(TEXT("area_size_y")) * 100.0f;
//     OutData.AttenuationRadius = LightObj->HasField(TEXT("cutoff_distance")) ? LightObj->GetNumberField(TEXT("cutoff_distance")) * 100.0f : 1000.0f;
//     OutData.Spread = LightObj->HasField(TEXT("spread")) ? FMath::RadiansToDegrees(LightObj->GetNumberField(TEXT("spread"))) : 180.0f;

//     return true;
// }

bool FLightManager::ParseSingleLight(const TSharedPtr<FJsonObject>& LightObj, FLightData& OutData)
{
    if (!LightObj.IsValid()) return false;

    OutData.Name = LightObj->GetStringField(TEXT("name"));
    OutData.Type = LightObj->GetStringField(TEXT("type"));
    OutData.bEnabled = LightObj->HasField(TEXT("enabled")) ? LightObj->GetBoolField(TEXT("enabled")) : true;
    OutData.bUseShadow = LightObj->HasField(TEXT("use_shadow")) ? LightObj->GetBoolField(TEXT("use_shadow")) : true;

    // --- 坐标系统转换 (Blender -> UE) ---
    const TArray<TSharedPtr<FJsonValue>>* LPos = nullptr;
    if (LightObj->TryGetArrayField(TEXT("location"), LPos) && LPos->Num() == 3)
    {
        float BX = (*LPos)[0]->AsNumber();
        float BY = (*LPos)[1]->AsNumber();
        float BZ = (*LPos)[2]->AsNumber();
        // UE: X=Forward(BY), Y=Right(BX), Z=Up(BZ). 单位 m -> cm.
        OutData.Location = FVector(BY * 100.0f, BX * 100.0f, BZ * 100.0f);
    }

    // --- 旋转坐标系转换 (Blender -> UE) ---
    const TArray<TSharedPtr<FJsonValue>>* LRot = nullptr;
    if (LightObj->TryGetArrayField(TEXT("rotation_euler"), LRot) && LRot->Num() == 3)
    {
        const float RX = (*LRot)[0]->AsNumber();
        const float RY = (*LRot)[1]->AsNumber();
        const float RZ = (*LRot)[2]->AsNumber();

        // 1. Blender XYZ Euler (rad) -> 四元数 (应用顺序: X then Y then Z)
        const FQuat Qx(FVector(1, 0, 0), RX);
        const FQuat Qy(FVector(0, 1, 0), RY);
        const FQuat Qz(FVector(0, 0, 1), RZ);
        FQuat BlenderQuat = Qz * Qy * Qx;

        // 2. 构建 Blender 旋转矩阵，提取基向量
        FMatrix BMat = FRotationMatrix::Make(BlenderQuat);
        FVector B_Right = BMat.GetUnitAxis(EAxis::X);
        FVector B_Fwd   = BMat.GetUnitAxis(EAxis::Y);
        FVector B_Up    = BMat.GetUnitAxis(EAxis::Z);

        // 3. 坐标轴映射: Blender(X,Y,Z) -> UE(Y,X,Z)
        //    Blender 右手系 Y-Forward -> UE 左手系 X-Forward
        //    同时翻转 Y 轴以修正手性: UE.Y = -B.X
        FVector UE_Fwd_Vec   = FVector( B_Fwd.Y,  -B_Fwd.X,   B_Fwd.Z);
        FVector UE_Right_Vec = FVector( B_Right.Y, -B_Right.X, B_Right.Z);
        FVector UE_Up_Vec    = FVector( B_Up.Y,   -B_Up.X,    B_Up.Z);

        // 4. 构建 UE 旋转矩阵并转为四元数
        FMatrix UEMat = FMatrix(UE_Fwd_Vec, UE_Right_Vec, UE_Up_Vec, FVector::ZeroVector);
        FQuat UEQuat = UEMat.ToQuat();

        // 5. 修正光源朝向偏移
        //    Blender Area Light 默认朝向 -Z 轴
        //    UE RectLight 默认朝向 +X 轴
        //    需要先绕 Y 轴旋转 +90° 将 +X 对齐到 +Z，再绕 X 轴旋转 180° 翻转到 -Z
        FQuat LightOffset = FRotator(90.0f, 0.0f, 180.0f).Quaternion();
        OutData.Rotation = (UEQuat * LightOffset).Rotator();
    }

    // --- 能量换算: Blender W (辐射通量) -> UE Lumens (光通量) ---
    // Blender energy 单位为瓦特 (W, radiometric flux)
    // UE Lumens 单位为流明 (lm, photometric flux)
    // 换算: lm = W × 683 (标准光效，白色光源近似)
    const float BlenderWatts = LightObj->GetNumberField(TEXT("energy"));
    OutData.Intensity = BlenderWatts * 2.0f; //683.0f;

    // --- 颜色 ---
    const TArray<TSharedPtr<FJsonValue>>* LCol = nullptr;
    if (LightObj->TryGetArrayField(TEXT("color"), LCol) && LCol->Num() == 3)
    {
        OutData.Color = FLinearColor(
            (*LCol)[0]->AsNumber(),
            (*LCol)[1]->AsNumber(),
            (*LCol)[2]->AsNumber()
        );
    }

    // --- 面积尺寸: m -> cm，area_size_y 缺失时回退为正方形 ---
    const float SizeX = LightObj->GetNumberField(TEXT("area_size"));
    const float SizeY = LightObj->HasField(TEXT("area_size_y"))
        ? LightObj->GetNumberField(TEXT("area_size_y"))
        : SizeX;
    OutData.SourceWidth  = SizeX * 100.0f;
    OutData.SourceHeight = SizeY * 100.0f;

    // --- 衰减半径 ---
    OutData.AttenuationRadius = LightObj->HasField(TEXT("cutoff_distance"))
        ? LightObj->GetNumberField(TEXT("cutoff_distance")) * 100.0f
        : 1000.0f;

    // --- Spread -> BarnDoorAngle ---
    // Blender spread: π = 全漫射(无遮挡), 0 = 完全聚光
    // UE BarnDoorAngle: 0° = 无遮挡, 88° = 最大遮挡
    // 反向线性映射: angle = (1 - spread/π) × 88
    const float SpreadRad = LightObj->HasField(TEXT("spread"))
        ? LightObj->GetNumberField(TEXT("spread"))
        : PI;
    OutData.Spread = FMath::Clamp((1.0f - SpreadRad / PI) * 88.0f, 0.0f, 88.0f);

    return true;
}

void FLightManager::ProcessLights(UWorld* World, const TArray<FLightData>& Lights)
{
    if (!World) return;

    for (const FLightData& LData : Lights)
    {
        ARectLight* TargetLight = nullptr;
        
        // 1. 查找现有灯光 (Label 或 Name)
        for (TActorIterator<ARectLight> It(World); It; ++It)
        {
            if (It->GetActorNameOrLabel().Equals(LData.Name) || It->GetName().Contains(LData.Name))
            {
                TargetLight = *It;
                break;
            }
        }

        // 2. 如果没找到且启用了，则创建新的
        if (!TargetLight && LData.bEnabled)
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            
            TargetLight = World->SpawnActor<ARectLight>(ARectLight::StaticClass(), LData.Location, LData.Rotation, SpawnParams);
            if (TargetLight)
            {
#if WITH_EDITOR
                TargetLight->SetActorLabel(LData.Name);
#endif
                UE_LOG(LogTemp, Warning, TEXT("LightManager: Created new RectLight: %s"), *LData.Name);
            }
        }

        // 3. 更新属性
        if (TargetLight)
        {
            TargetLight->SetActorLocationAndRotation(LData.Location, LData.Rotation);
            TargetLight->SetActorHiddenInGame(!LData.bEnabled);
            
            // 设置移动性为 Movable，否则无法在运行时更新坐标
            if (TargetLight->GetRootComponent())
            {
                TargetLight->GetRootComponent()->SetMobility(EComponentMobility::Movable);
            }

            URectLightComponent* LightComp = Cast<URectLightComponent>(TargetLight->GetLightComponent());
            if (LightComp)
            {
                // 设置强度单位为 Lumens，不再乘额外的倍率系数
                LightComp->IntensityUnits = ELightUnits::Lumens;
                //LightComp->IntensityUnits = ELightUnits::Candelas;
                LightComp->SetIntensity(LData.Intensity * 1.0);
                //LightComp->SetIntensity(LData.Intensity * 683/ (LData.SourceWidth * LData.SourceHeight / 1.0));
                LightComp->SetLightColor(LData.Color);
                LightComp->SourceWidth = LData.SourceWidth;
                LightComp->SourceHeight = LData.SourceHeight;
                LightComp->AttenuationRadius = LData.AttenuationRadius;
                // LightComp->BarnDoorAngle = FMath::Clamp(LData.Spread, 0.0f, 88.0f); // UE 矩形灯遮板角度最大约 88-90
                LightComp->BarnDoorAngle = LData.Spread;
                LightComp->CastShadows = LData.bUseShadow;
                
                // 降低间接光强度，平衡 Lumen GI 效果 (之前是 5.0 容易导致过曝或发光感)
                LightComp->IndirectLightingIntensity = 0.5f;//1.5f;1.5导致过曝光
                
                LightComp->SetVisibility(LData.bEnabled);
                LightComp->MarkRenderStateDirty();
                UE_LOG(LogTemp, Warning, TEXT("LightManager: Correctly updated light: %s (Intensity: %f Lumens)"), *LData.Name, LData.Intensity);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("LightManager: Failed to get/cast RectLightComponent for light: %s"), *LData.Name);
            }
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("LightManager: Failed to spawn/find light: %s"), *LData.Name);
        }
    }
}
