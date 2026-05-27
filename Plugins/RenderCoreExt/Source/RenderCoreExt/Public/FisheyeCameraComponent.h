#pragma once

#include "CoreMinimal.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Engine/TextureRenderTargetCube.h"
#include "FisheyeCameraComponent.generated.h"

/**
 * 多视角渲染模式
 */
UENUM(BlueprintType)
enum class EMultiViewRenderMode : uint8
{
    Serial UMETA(DisplayName = "Serial (Low Memory)"),
    Parallel UMETA(DisplayName = "Parallel (High Speed)")
};

/**
 * 存储渲染探针及其配套 RT 的结构 (并行渲染核心)
 */
USTRUCT(BlueprintType)
struct FMultiViewProbe
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    USceneCaptureComponent2D* CaptureComp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    UTextureRenderTarget2D* DepthRT;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    UTextureRenderTarget2D* RGBRT;

    FMultiViewProbe() : CaptureComp(nullptr), DepthRT(nullptr), RGBRT(nullptr) {}
};

/**
 * 存储单个 View 的元数据 (用于 Shader 采样)
 */
USTRUCT(BlueprintType)
struct FMultiViewData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    int32 ViewIndex;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    FRotator Rotation;

    FMultiViewData() : ViewIndex(0), Rotation(FRotator::ZeroRotator) {}
};

/**
 * 鱼眼相机参数结构体
 */
USTRUCT(BlueprintType)
struct FFisheyeParams
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float Fx = 500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float Fy = 500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float Cx = 640.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float Cy = 360.0f;

    // Kannala-Brandt 畸变系数 (示例)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float K1 = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float K2 = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float K3 = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float K4 = 0.0f;

    // 新增参数
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    FIntPoint Resolution = FIntPoint(1280, 720);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    FString CameraModel = TEXT("pinhole");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    TArray<float> Intrinsics; // [fx, fy, cx, cy]

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    TArray<float> DistortionCoeffs; // [k1, k2, k3, k4]

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float ExposureBias = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    bool bEnableToneCurve = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    bool bUseHDR = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    int32 WarmUpFrames = 10;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    float SupersamplingMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
	FString RGBMaterialPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
	FString DepthMaterialPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
	FString PositionMaterialPath;

    
    // MultiView 模式参数
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|MultiView")
    float AngleStep = 10.0f; // 角度步长

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|MultiView")
    float HorizontalFOV = 180.0f; // 水平覆盖范围

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|MultiView")
    float VerticalFOV = 120.0f;   // 垂直覆盖范围

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    bool bEnableShadows = true; // 是否启用动态阴影

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|MultiView")
    EMultiViewRenderMode RenderMode = EMultiViewRenderMode::Serial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|MultiView")
    int32 NumParallelProbes = 4; // 并行渲染时使用的探针数量
};

// ============================================================
// Dome LightStage 数据结构
// ============================================================

/**
 * 穹顶相机分布模式
 */
UENUM(BlueprintType)
enum class EDomeCameraLayout : uint8
{
    FibonacciSphere UMETA(DisplayName = "Fibonacci Sphere (均匀分布)"),
    Rings           UMETA(DisplayName = "Ring-Based (环形层)"),
    Manual          UMETA(DisplayName = "Manual (手动指定)")
};

/**
 * 渲染通道开关标志位 (可组合)
 */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsBitMaskValues = "true"))
enum class EDomeRenderPass : uint8
{
    None     = 0         UMETA(Hidden),
    RGB      = 0x01      UMETA(DisplayName = "RGB"),
    Depth    = 0x02      UMETA(DisplayName = "Depth"),
    Normal   = 0x04      UMETA(DisplayName = "Normal"),
    Position = 0x08      UMETA(DisplayName = "Position"),
    Semantic = 0x10      UMETA(DisplayName = "Semantic")
};
ENUM_CLASS_FLAGS(EDomeRenderPass);

/**
 * 单个穹顶相机视角描述
 */
USTRUCT(BlueprintType)
struct FDomeCameraView
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    int32 CameraIndex = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    FString CameraName; // e.g. "Cam_Ring1_03"

    // 相对 Dome 中心 Actor 的位置偏移 (cm)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    FVector RelativePosition = FVector::ZeroVector;

    // 相机朝向旋转 (朝向 Dome 中心)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    FRotator LookAtRotation = FRotator::ZeroRotator;

    // 单个相机 FOV (度)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    float FOV = 50.0f;

    FDomeCameraView() {}
};

/**
 * 穹顶相机阵列整体配置
 */
USTRUCT(BlueprintType)
struct FDomeCameraConfig
{
    GENERATED_BODY()

    // 穹顶球半径 (cm)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    float DomeRadius = 200.0f;

    // 相机总数 (Fibonacci / Rings 模式自动计算)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    int32 NumCameras = 32;

    // 相机分布算法
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    EDomeCameraLayout Layout = EDomeCameraLayout::FibonacciSphere;

    // 单个相机 FOV (度)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    float CameraFOV = 50.0f;

    // 渲染分辨率 (每个相机)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    FIntPoint Resolution = FIntPoint(2048, 2048);

    // 曝光补偿
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    float ExposureBias = 0.0f;

    // 是否启用色调映射
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    bool bEnableToneCurve = true;

    // 预热帧数
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    int32 WarmUpFrames = 5;

    // 并行渲染批次大小 (每批同时提交的相机数量)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    int32 RenderBatchSize = 4;

    // 启用的渲染通道 (EDomeRenderPass 的位标志组合)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome", meta = (Bitmask, BitmaskEnum = "/Script/RenderCoreExt.EDomeRenderPass"))
    uint8 EnabledPasses = 0x1F; // 默认全开 (RGB|Depth|Normal|Position|Semantic)

    // 是否启用动态阴影
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    bool bEnableShadows = true;

    // 仅覆盖上半球 (Pitch >= 0)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome")
    bool bUpperHemisphereOnly = true;

    // Ring 模式：每环相机数
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome|Rings")
    TArray<int32> CamerasPerRing; // e.g. [8, 12, 8, 4]

    // Ring 模式：每环仰角 (度, 0=赤道, 90=顶点)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome|Rings")
    TArray<float> RingElevations; // e.g. [15.0, 35.0, 55.0, 80.0]

    // Manual 模式：手动指定相机列表
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome|Manual")
    TArray<FDomeCameraView> ManualCameras;

    // 后处理材质路径 (由 Python 动态加载)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome|Materials")
    FString DepthMaterialPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome|Materials")
    FString NormalMaterialPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome|Materials")
    FString PositionMaterialPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dome|Materials")
    FString SemanticMaterialPath;

    FDomeCameraConfig()
    {
        // 默认 Ring 布局：4 环 (15°/35°/55°/80°)
        CamerasPerRing = {8, 12, 8, 4};
        RingElevations = {15.0f, 35.0f, 55.0f, 80.0f};
    }
};

/**
 * 自定义鱼眼相机组件
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class RENDERCOREEXT_API UFisheyeCameraComponent : public USceneCaptureComponent2D
{
    GENERATED_BODY()

public:
    UFisheyeCameraComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    FFisheyeParams FisheyeParameters;

    // 后处理材质
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    UMaterialInterface* FisheyePostProcessMaterial;

    // 深度后处理材质 (输出 SceneDepth 到 Emissive)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    UMaterialInterface* FisheyeDepthPostProcessMaterial;

    // 世界坐标后处理材质 (输出 WorldPosition 到 Emissive)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    UMaterialInterface* FisheyePositionPostProcessMaterial;

    // 法线后处理材质 (输出 WorldNormal 到 Emissive) — Dome LightStage 使用
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    UMaterialInterface* FisheyeNormalPostProcessMaterial;

    // 语义分割后处理材质 (输出 CustomStencil 到颜色) — Dome LightStage 使用
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye")
    UMaterialInterface* FisheyeSemanticPostProcessMaterial;

    // 由Python调用
    UFUNCTION(BlueprintCallable, Category = "Fisheye")
    virtual void ConfigureCamera(const FFisheyeParams& Params);

    UFUNCTION(BlueprintCallable, Category = "Fisheye")
    virtual void CaptureFisheyeScene();

    UFUNCTION(BlueprintPure, Category = "Fisheye")
    bool IsBusy() const { return bIsCapturingSequence; }

    // 将像素坐标转换为射线方向
    UFUNCTION(BlueprintCallable, Category = "Fisheye")
    FVector PixelToRay(FVector2D PixelCoord) const;

    // ========== 导出功能 ==========
    
    // 保存RGB图像
    UFUNCTION(BlueprintCallable, Category = "Fisheye|Export")
    virtual bool SaveRGBImage(const FString& FilePath, int32 FrameNumber = -1);

    // 保存深度图（8位PNG，精度较低）
    UFUNCTION(BlueprintCallable, Category = "Fisheye|Export")
    virtual bool SaveDepthImage(const FString& FilePath, int32 FrameNumber = -1);

    // 保存高精度深度图（16位PNG或EXR格式）
    UFUNCTION(BlueprintCallable, Category = "Fisheye|Export")
    virtual bool SaveDepthImageHighPrecision(const FString& FilePath, int32 FrameNumber = -1, bool bSaveAsEXR = false);

    // 保存深度图为二进制文件（.raw格式，最高精度）
    UFUNCTION(BlueprintCallable, Category = "Fisheye|Export")
    virtual bool SaveDepthImageRaw(const FString& FilePath, int32 FrameNumber = -1);

    // 保存位姿（位置和旋转）
    UFUNCTION(BlueprintCallable, Category = "Fisheye|Export")
    virtual bool SavePose(const FString& FilePath, int32 FrameNumber = -1);

    // 保存所有数据（RGB + Depth + Pose）
    UFUNCTION(BlueprintCallable, Category = "Fisheye|Export")
    virtual bool SaveAllData(const FString& BasePath, int32 FrameNumber = -1);

    // 获取深度渲染目标
    UPROPERTY(BlueprintReadOnly, Category = "Fisheye")
    UTextureRenderTarget2D* DepthRenderTarget;

    // ========== Cubemap 捕获 ==========
    
    // 深度Cubemap渲染目标
    UPROPERTY(BlueprintReadOnly, Category = "Fisheye|Cubemap")
    UTextureRenderTargetCube* DepthCubemapRenderTarget;

    // RGB Cubemap渲染目标
    UPROPERTY(BlueprintReadOnly, Category = "Fisheye|Cubemap")
    UTextureRenderTargetCube* RGBCubemapRenderTarget;
    
    // Cubemap捕获组件 (运行时创建)
    UPROPERTY()
    USceneCaptureComponentCube* DepthCubeCaptureComponent;

    UPROPERTY()
    USceneCaptureComponentCube* RGBCubeCaptureComponent;
    
    // 从Cubemap采样的深度后处理材质
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|Cubemap")
    UMaterialInterface* FisheyeDepthFromCubemapMaterial;

    // 从Cubemap采样的RGB后处理材质
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|Cubemap")
    UMaterialInterface* FisheyeRGBFromCubemapMaterial;

    // 动态材质实例缓存
    UPROPERTY()
    UMaterialInstanceDynamic* DepthCubemapMID;

    UPROPERTY()
    UMaterialInstanceDynamic* RGBCubemapMID;
    
    // 是否使用Cubemap方式捕获 (推荐开启以获取正确的鱼眼几何)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|Cubemap")
    bool bUseCubemapForDepth = true;
    
    // Cubemap分辨率 (每个面的分辨率，默认512)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|Cubemap")
    int32 CubemapResolution = 512;

    // 保存目录设置
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|Export")
    FString ExportBasePath = TEXT("C:/UE5_Exports/");

    // 是否自动保存每一帧
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|Export")
    bool bAutoSaveOnCapture = false;

    // 帧计数器
    UPROPERTY(BlueprintReadOnly, Category = "Fisheye|Export")
    int32 FrameCounter = 0;

    // 深度捕获模式：true=尝试转换为线性深度，false=z-buffer深度（非线性，0-1）
    // 注意：UE5中SCS_SceneDepth捕获的是z-buffer深度，线性深度转换需要在后处理材质中实现
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|Export")
    bool bUseLinearDepth = false;  // 默认使用z-buffer深度（更稳定）

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|PostProcess")
    float ExposureBias = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|PostProcess")
    bool bEnableToneCurve = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|PostProcess")
    bool bUseHDR = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|PostProcess")
    int32 WarmUpFrames = 10;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fisheye|PostProcess")
    float SupersamplingMultiplier = 1.0f;

    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
    virtual void BeginPlay() override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    // 同步参数到渲染目标和材质
    void SyncParameters();

private:
    // 内部辅助函数：从RenderTarget读取像素数据
    bool ReadRenderTargetPixels(UTextureRenderTarget2D* RenderTarget, TArray<FColor>& OutPixels);
    bool ReadDepthRenderTargetPixels(UTextureRenderTarget2D* RenderTarget, TArray<float>& OutDepthValues);
    
    // 高精度深度读取（使用Float16或Float32）
    bool ReadDepthRenderTargetPixelsHighPrecision(UTextureRenderTarget2D* RenderTarget, TArray<float>& OutDepthValues);
    
    // 保存位姿到文件（JSON格式）
    void WritePoseToFile(const FString& FilePath, const FVector& Location, const FQuat& Rotation, int32 FrameNumber);
    
    // Cubemap深度捕获辅助函数
    void CaptureDepthCubemap();
    void RenderFisheyeDepthFromCubemap();
    void CaptureRGBCubemap();
    void RenderFisheyeRGBFromCubemap();

    // 预热状态变量
    int32 RemainingWarmUpFrames = 0;
    bool bIsCapturingSequence = false;
    FString PendingExportBasePath;
    int32 PendingFrameNumber = -1;
};
