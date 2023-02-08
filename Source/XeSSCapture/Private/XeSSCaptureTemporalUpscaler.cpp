#include "XeSSCaptureTemporalUpscaler.h"

#include "PostProcess/PostProcessing.h"
#include "HighResScreenshot.h"
#include "PostProcess/PostProcessMaterial.h"
#include "PostProcess/PostProcessDownsample.h"
#include "PostProcess/PostProcessMitchellNetravali.h"
#include "ImagePixelData.h"
#include "ImageWriteStream.h"
#include "ImageWriteTask.h"
#include "ImageWriteQueue.h"
#include "HighResScreenshot.h"
#include "SceneTextureParameters.h"

#include "Misc/FileHelper.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define CAPTURE_SR_SKIP_CODE	(true)

const TCHAR* const kTAAOutputNames[] = {
	TEXT("TemporalAA"),
	TEXT("TemporalAA"),
	TEXT("TemporalAA"),
	TEXT("SSRTemporalAA"),
	TEXT("LightShaftTemporalAA"),
	TEXT("DOFTemporalAA"),
	TEXT("DOFTemporalAA"),
};

const TCHAR* const kTAAPassNames[] = {
	TEXT("Main"),
	TEXT("MainUpsampling"),
	TEXT("MainSuperSampling"),
	TEXT("ScreenSpaceReflections"),
	TEXT("LightShaft"),
	TEXT("DOF"),
	TEXT("DOFUpsampling"),
};

const int32 GTemporalAATileSizeX = 8;
const int32 GTemporalAATileSizeY = 8;
const int32 GXeSSTileSizeX = FComputeShaderUtils::kGolden2DGroupSize;
const int32 GXeSSTileSizeY = FComputeShaderUtils::kGolden2DGroupSize;

static TUniquePtr<FImagePixelData> ReadbackPixelData(FRHICommandListImmediate& RHICmdList, FRHITexture* Texture, FIntRect SourceRect)
{
	check(Texture);
	check(Texture->GetTexture2D());

	const int32 MSAAXSamples = Texture->GetNumSamples();
	SourceRect.Min.X *= MSAAXSamples;
	SourceRect.Max.X *= MSAAXSamples;

	switch (Texture->GetFormat())
	{
		case PF_FloatRGBA:
		{
			TArray<FFloat16Color> RawPixels;
			RawPixels.SetNum(SourceRect.Width() * SourceRect.Height());
			RHICmdList.ReadSurfaceFloatData(Texture, SourceRect, RawPixels, (ECubeFace)0, 0, 0);
			TUniquePtr<TImagePixelData<FFloat16Color>> PixelData = MakeUnique<TImagePixelData<FFloat16Color>>(SourceRect.Size(), TArray64<FFloat16Color>(MoveTemp(RawPixels)));

			check(PixelData->IsDataWellFormed());
			return PixelData;
		}

		case PF_A32B32G32R32F:
		{
			FReadSurfaceDataFlags ReadDataFlags(RCM_MinMax);
			ReadDataFlags.SetLinearToGamma(false);

			TArray<FLinearColor> RawPixels;
			RawPixels.SetNum(SourceRect.Width() * SourceRect.Height());
			RHICmdList.ReadSurfaceData(Texture, SourceRect, RawPixels, ReadDataFlags);
			TUniquePtr<TImagePixelData<FLinearColor>> PixelData = MakeUnique<TImagePixelData<FLinearColor>>(SourceRect.Size(), TArray64<FLinearColor>(MoveTemp(RawPixels)));

			check(PixelData->IsDataWellFormed());
			return PixelData;
		}

		case PF_R8G8B8A8:
		case PF_B8G8R8A8:
		{
			FReadSurfaceDataFlags ReadDataFlags;
			ReadDataFlags.SetLinearToGamma(false);

			TArray<FColor> RawPixels;
			RawPixels.SetNum(SourceRect.Width() * SourceRect.Height());
			RHICmdList.ReadSurfaceData(Texture, SourceRect, RawPixels, ReadDataFlags);
			TUniquePtr<TImagePixelData<FColor>> PixelData = MakeUnique<TImagePixelData<FColor>>(SourceRect.Size(), TArray64<FColor>(MoveTemp(RawPixels)));

			check(PixelData->IsDataWellFormed());
			return PixelData;
		}
	}

	return nullptr;
}

static void AddDumpToExrPass(FRDGBuilder& GraphBuilder, FScreenPassTexture Input, const FString& Filename)
{
	check(Input.IsValid());

	FHighResScreenshotConfig& HighResScreenshotConfig = GetHighResScreenshotConfig();

	if (!ensureMsgf(HighResScreenshotConfig.ImageWriteQueue, TEXT("Unable to write images unless FHighResScreenshotConfig::Init has been called.")))
	{
		return;
	}

	if (GIsHighResScreenshot && HighResScreenshotConfig.CaptureRegion.Area())
	{
		Input.ViewRect = HighResScreenshotConfig.CaptureRegion;
	}

	AddReadbackTexturePass(GraphBuilder, RDG_EVENT_NAME("DumpToFile(%s)", Input.Texture->Name), Input.Texture,
		[&HighResScreenshotConfig, Input, Filename](FRHICommandListImmediate& RHICmdList)
	{
		TUniquePtr<FImagePixelData> PixelData = ReadbackPixelData(RHICmdList, Input.Texture->GetRHI(), Input.ViewRect);

		if (!PixelData.IsValid())
		{
			return;
		}

		TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
		ImageTask->PixelData = MoveTemp(PixelData);

		HighResScreenshotConfig.PopulateImageTaskParams(*ImageTask);
		ImageTask->Filename = Filename;
		ImageTask->Format = EImageFormat::EXR;

		if (ImageTask->PixelData->GetType() == EImagePixelType::Color)
		{
			// Always write full alpha
			ImageTask->PixelPreProcessors.Add(TAsyncAlphaWrite<FColor>(255));

			if (ImageTask->Format == EImageFormat::EXR)
			{
				// Write FColors with a gamma curve. This replicates behavior that previously existed in ExrImageWrapper.cpp (see following overloads) that assumed
				// any 8 bit output format needed linearizing, but this is not a safe assumption to make at such a low level:
				// void ExtractAndConvertChannel(const uint8*Src, uint32 SrcChannels, uint32 x, uint32 y, float* ChannelOUT)
				// void ExtractAndConvertChannel(const uint8*Src, uint32 SrcChannels, uint32 x, uint32 y, FFloat16* ChannelOUT)
				ImageTask->PixelPreProcessors.Add(TAsyncGammaCorrect<FColor>(2.2f));
			}
		}

		HighResScreenshotConfig.ImageWriteQueue->Enqueue(MoveTemp(ImageTask));
	});
}

class FXeSSVelocityFlattenCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FXeSSVelocityFlattenCS);
	SHADER_USE_PARAMETER_STRUCT(FXeSSVelocityFlattenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
#if ENGINE_MAJOR_VERSION < 5
		SHADER_PARAMETER(FVector4, OutputViewportSize)
		SHADER_PARAMETER(FVector4, OutputViewportRect)
#else
		SHADER_PARAMETER(FVector4f, OutputViewportSize)
		SHADER_PARAMETER(FVector4f, OutputViewportRect)
#endif

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferVelocityTexture)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		// Temporal upsample specific parameters.
#if ENGINE_MAJOR_VERSION < 5
		SHADER_PARAMETER(FVector2D, InputViewMin)
		SHADER_PARAMETER(FVector4, InputViewSize)
		SHADER_PARAMETER(FVector2D, TemporalJitterPixels)
#else
		SHADER_PARAMETER(FVector2f, InputViewMin)
		SHADER_PARAMETER(FVector4f, InputViewSize)
		SHADER_PARAMETER(FVector2f, TemporalJitterPixels)
#endif

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVelocityTex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), GXeSSTileSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), GXeSSTileSizeY);
	}
}; // class FXeSSVelocityFlattenCS

IMPLEMENT_GLOBAL_SHADER(FXeSSVelocityFlattenCS, "/Plugin/XeSSCapture/Private/FlattenVelocity.usf", "MainCS", SF_Compute);

static FRDGTextureRef AddVelocityFlatteningXeSSPass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef InSceneDepthTexture,
	FRDGTextureRef InVelocityTexture,
	const FViewInfo& View)
{
	check(InSceneDepthTexture);
	check(InVelocityTexture);

	// Src rectangle.
	const FIntRect SrcRect = View.ViewRect;
	const FIntRect DestRect = FIntRect(FIntPoint::ZeroValue, View.GetSecondaryViewRectSize());

	FRDGTextureDesc SceneVelocityDesc = FRDGTextureDesc::Create2D(
		DestRect.Size(),
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTexture* OutputVelocityTexture = GraphBuilder.CreateTexture(
		SceneVelocityDesc,
		TEXT("XeSS Capture Upscaled Velocity Texture"),
		ERDGTextureFlags::MultiFrame);

	{
		FXeSSVelocityFlattenCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FXeSSVelocityFlattenCS::FParameters>();

		// Setups common shader parameters
		const FIntRect InputViewRect = SrcRect;

		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

		PassParameters->SceneDepthTexture = InSceneDepthTexture;
		PassParameters->GBufferVelocityTexture = InVelocityTexture;

		// We need a valid velocity buffer texture. Use black (no velocity) if none exists.
		if (!PassParameters->GBufferVelocityTexture)
		{
			PassParameters->GBufferVelocityTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);;
		}

#if ENGINE_MAJOR_VERSION < 5
		PassParameters->OutputViewportSize = FVector4(
			DestRect.Width(), DestRect.Height(), 1.0f / float(DestRect.Width()), 1.0f / float(DestRect.Height()));
		PassParameters->OutputViewportRect = FVector4(DestRect.Min.X, DestRect.Min.Y, DestRect.Max.X, DestRect.Max.Y);
#else
		PassParameters->OutputViewportSize = FVector4f(
			DestRect.Width(), DestRect.Height(), 1.0f / float(DestRect.Width()), 1.0f / float(DestRect.Height()));
		PassParameters->OutputViewportRect = FVector4f(DestRect.Min.X, DestRect.Min.Y, DestRect.Max.X, DestRect.Max.Y);
#endif

		// Temporal upsample specific shader parameters.
		{
#if ENGINE_MAJOR_VERSION < 5
			PassParameters->TemporalJitterPixels = View.TemporalJitterPixels;
			PassParameters->InputViewMin = FVector2D(InputViewRect.Min.X, InputViewRect.Min.Y);
			PassParameters->InputViewSize = FVector4(
				InputViewRect.Width(), InputViewRect.Height(), 1.0f / InputViewRect.Width(), 1.0f / InputViewRect.Height());
#else
			PassParameters->TemporalJitterPixels = FVector2f(View.TemporalJitterPixels);
			PassParameters->InputViewMin = FVector2f(InputViewRect.Min.X, InputViewRect.Min.Y);
			PassParameters->InputViewSize = FVector4f(
				InputViewRect.Width(), InputViewRect.Height(), 1.0f / InputViewRect.Width(), 1.0f / InputViewRect.Height());
#endif
		}

		// UAVs
		{
			PassParameters->OutVelocityTex = GraphBuilder.CreateUAV(OutputVelocityTexture);
		}

		TShaderMapRef<FXeSSVelocityFlattenCS> ComputeShader(View.ShaderMap);

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("XeSSCapture %s %dx%d -> %dx%d",
				TEXT("Velocity Flattening"),
				SrcRect.Width(), SrcRect.Height(),
				DestRect.Width(), DestRect.Height()),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DestRect.Size(), GXeSSTileSizeX));
	}

	return OutputVelocityTexture;
}

class FLowResXeSSVelocityCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLowResXeSSVelocityCS);
	SHADER_USE_PARAMETER_STRUCT(FLowResXeSSVelocityCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferVelocityTexture)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		// Temporal upsample specific parameters.
#if ENGINE_MAJOR_VERSION < 5
		SHADER_PARAMETER(FVector2D, InputViewMin)
		SHADER_PARAMETER(FVector4, InputViewSize)
		SHADER_PARAMETER(FVector2D, TemporalJitterPixels)
#else
		SHADER_PARAMETER(FVector2f, InputViewMin)
		SHADER_PARAMETER(FVector4f, InputViewSize)
		SHADER_PARAMETER(FVector2f, TemporalJitterPixels)
#endif

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVelocityTex)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), GXeSSTileSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), GXeSSTileSizeY);
	}
}; // class FLowResXeSSVelocityCS

IMPLEMENT_GLOBAL_SHADER(FLowResXeSSVelocityCS, "/Plugin/XeSSCapture/Private/CaptureLowResVelocity.usf", "MainCS", SF_Compute);

static FRDGTextureRef AddLowResVelocityXeSSPass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef InSceneDepthTexture,
	FRDGTextureRef InVelocityTexture,
	const FViewInfo& View)
{
	check(InSceneDepthTexture);
	check(InVelocityTexture);

	// Dst rectangle.
	const FIntRect DestRect = FIntRect(FIntPoint::ZeroValue, View.ViewRect.Size());

	FRDGTextureDesc SceneVelocityDesc = FRDGTextureDesc::Create2D(
		DestRect.Size(),
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTexture* OutputVelocityTexture = GraphBuilder.CreateTexture(
		SceneVelocityDesc,
		TEXT("XeSS Capture Low-Res Velocity Texture"),
		ERDGTextureFlags::MultiFrame);

	{
		FLowResXeSSVelocityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLowResXeSSVelocityCS::FParameters>();

		// Setups common shader parameters
		const FIntRect InputViewRect = View.ViewRect;

		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

		PassParameters->SceneDepthTexture = InSceneDepthTexture;
		PassParameters->GBufferVelocityTexture = InVelocityTexture;

		// We need a valid velocity buffer texture. Use black (no velocity) if none exists.
		if (!PassParameters->GBufferVelocityTexture)
		{
			PassParameters->GBufferVelocityTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);;
		}

		// Temporal upsample specific shader parameters.
		{
#if ENGINE_MAJOR_VERSION < 5
			PassParameters->TemporalJitterPixels = View.TemporalJitterPixels;
			PassParameters->InputViewMin = FVector2D(InputViewRect.Min.X, InputViewRect.Min.Y);
			PassParameters->InputViewSize = FVector4(
				InputViewRect.Width(), InputViewRect.Height(), 1.0f / InputViewRect.Width(), 1.0f / InputViewRect.Height());
#else
			PassParameters->TemporalJitterPixels = FVector2f(View.TemporalJitterPixels);
			PassParameters->InputViewMin = FVector2f(InputViewRect.Min.X, InputViewRect.Min.Y);
			PassParameters->InputViewSize = FVector4f(
				InputViewRect.Width(), InputViewRect.Height(), 1.0f / InputViewRect.Width(), 1.0f / InputViewRect.Height());
#endif
		}

		// UAVs
		{
			PassParameters->OutVelocityTex = GraphBuilder.CreateUAV(OutputVelocityTexture);
		}

		TShaderMapRef<FLowResXeSSVelocityCS> ComputeShader(View.ShaderMap);

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("XeSSCapture %s %dx%d",
				TEXT("Low-Res Velocity"),
				DestRect.Width(), DestRect.Height()),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DestRect.Size(), GXeSSTileSizeX));
	}

	return OutputVelocityTexture;
}

class FXeSSDepthCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FXeSSDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FXeSSDepthCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)

		// Temporal upsample specific parameters.
#if ENGINE_MAJOR_VERSION < 5
		SHADER_PARAMETER(FVector2D, InputViewMin)
		SHADER_PARAMETER(FVector4, InputViewSize)
		SHADER_PARAMETER(FVector2D, TemporalJitterPixels)
#else
		SHADER_PARAMETER(FVector2f, InputViewMin)
		SHADER_PARAMETER(FVector4f, InputViewSize)
		SHADER_PARAMETER(FVector2f, TemporalJitterPixels)
#endif

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), GXeSSTileSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), GXeSSTileSizeY);
	}
}; // class FXeSSDepthCS

IMPLEMENT_GLOBAL_SHADER(FXeSSDepthCS, "/Plugin/XeSSCapture/Private/CaptureDepth.usf", "MainCS", SF_Compute);

static FRDGTextureRef AddDepthXeSSPass(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef InSceneDepthTexture,
	const FViewInfo& View)
{
	check(InSceneDepthTexture);

	// Dst rectangle.
	const FIntRect DestRect = FIntRect(FIntPoint::ZeroValue, View.ViewRect.Size());

	FRDGTextureDesc SceneVelocityDesc = FRDGTextureDesc::Create2D(
		DestRect.Size(),
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTexture* OutputDepthTexture = GraphBuilder.CreateTexture(
		SceneVelocityDesc,
		TEXT("XeSS Capture Depth Texture"),
		ERDGTextureFlags::MultiFrame);

	{
		FXeSSDepthCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FXeSSDepthCS::FParameters>();

		// Setups common shader parameters
		const FIntRect InputViewRect = View.ViewRect;

		PassParameters->SceneDepthTexture = InSceneDepthTexture;

		// Temporal upsample specific shader parameters.
		{
#if ENGINE_MAJOR_VERSION < 5
			PassParameters->TemporalJitterPixels = View.TemporalJitterPixels;
			PassParameters->InputViewMin = FVector2D(InputViewRect.Min.X, InputViewRect.Min.Y);
			PassParameters->InputViewSize = FVector4(
				InputViewRect.Width(), InputViewRect.Height(), 1.0f / InputViewRect.Width(), 1.0f / InputViewRect.Height());
#else
			PassParameters->TemporalJitterPixels = FVector2f(View.TemporalJitterPixels);
			PassParameters->InputViewMin = FVector2f(InputViewRect.Min.X, InputViewRect.Min.Y);
			PassParameters->InputViewSize = FVector4f(
				InputViewRect.Width(), InputViewRect.Height(), 1.0f / InputViewRect.Width(), 1.0f / InputViewRect.Height());
#endif
		}

		// UAVs
		{
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputDepthTexture);
		}

		TShaderMapRef<FXeSSDepthCS> ComputeShader(View.ShaderMap);

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("XeSSCapture %s %dx%d",
				TEXT("Depth"),
				DestRect.Width(), DestRect.Height()),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DestRect.Size(), GXeSSTileSizeX));
	}

	return OutputDepthTexture;
}

class FXeSSSampleCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FXeSSSampleCS);
	SHADER_USE_PARAMETER_STRUCT(FXeSSSampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
#if ENGINE_MAJOR_VERSION < 5
		SHADER_PARAMETER(FVector2D, TemporalJitterPixels)
#else
		SHADER_PARAMETER(FVector2f, TemporalJitterPixels)
#endif
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), GXeSSTileSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), GXeSSTileSizeY);
	}
};

IMPLEMENT_GLOBAL_SHADER(FXeSSSampleCS, "/Plugin/XeSSCapture/Private/CaptureSample.usf", "MainCS", SF_Compute);

static FRDGTextureRef AddSamplePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View)
{
	const FIntRect DestRect = FIntRect(FIntPoint::ZeroValue, View.ViewRect.Size());

	FRDGTextureDesc SceneSampleDesc = FRDGTextureDesc::Create2D(
		DestRect.Size(),
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTexture* OutputSampleTexture = GraphBuilder.CreateTexture(
		SceneSampleDesc,
		TEXT("XeSS Capture Sample Texture"),
		ERDGTextureFlags::MultiFrame);

	// cs
	FXeSSSampleCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FXeSSSampleCS::FParameters>();

	// set pass inputs
#if ENGINE_MAJOR_VERSION < 5
	PassParameters->TemporalJitterPixels = View.TemporalJitterPixels;
#else
	PassParameters->TemporalJitterPixels = FVector2f(View.TemporalJitterPixels);
#endif
	PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputSampleTexture);

	TShaderMapRef<FXeSSSampleCS> ComputeShader(View.ShaderMap);
	ClearUnusedGraphResources(ComputeShader, PassParameters);

	FComputeShaderUtils::AddPass(GraphBuilder,
		RDG_EVENT_NAME("XeSSCapture %s",
			TEXT("Sample")),
		ComputeShader, PassParameters,
		FComputeShaderUtils::GetGroupCount(DestRect.Size(), GXeSSTileSizeX)
	);

	return OutputSampleTexture;
}

/** Configuration of TAA. */
struct FTAANoBlendPassParameters
{
	// TAA pass to run.
	ETAAPassConfig Pass = ETAAPassConfig::Main;

	// Whether to use the faster shader permutation.
	bool bUseFast = false;

	// Whether output texture should be render targetable.
	bool bOutputRenderTargetable = false;

	// Whether downsampled (box filtered, half resolution) frame should be written out.
	bool bDownsample = false;
	EPixelFormat DownsampleOverrideFormat = PF_Unknown;

	// Viewport rectangle of the input and output of TAA at ResolutionDivisor == 1.
	FIntRect InputViewRect;
	FIntRect OutputViewRect;

	// Resolution divisor.
	int32 ResolutionDivisor = 1;

	// Full resolution depth and velocity textures to reproject the history.
	FRDGTexture* SceneDepthTexture = nullptr;
	FRDGTexture* SceneVelocityTexture = nullptr;

	// Anti aliased scene color.
	// Can have alpha channel, or CoC for DOF.
	FRDGTexture* SceneColorInput = nullptr;

	// Optional information that get anti aliased, such as separate CoC for DOF.
	FRDGTexture* SceneMetadataInput = nullptr;


	FTAANoBlendPassParameters(const FViewInfo& View)
		: InputViewRect(View.ViewRect)
		, OutputViewRect(View.ViewRect)
	{ }


	// Customizes the view rectangles for input and output.
	FORCEINLINE void SetupViewRect(const FViewInfo& View, int32 InResolutionDivisor = 1)
	{
		ResolutionDivisor = InResolutionDivisor;

		InputViewRect = View.ViewRect;

		// When upsampling, always upsampling to top left corner to reuse same RT as before upsampling.
		if (IsTAAUpsamplingConfig(Pass))
		{
			OutputViewRect.Min = FIntPoint(0, 0);
			OutputViewRect.Max = View.GetSecondaryViewRectSize();
		}
		else
		{
			OutputViewRect = InputViewRect;
		}
	}

	// Shifts input and output view rect to top left corner
	FORCEINLINE void TopLeftCornerViewRects()
	{
		InputViewRect.Max -= InputViewRect.Min;
		InputViewRect.Min = FIntPoint::ZeroValue;
		OutputViewRect.Max -= OutputViewRect.Min;
		OutputViewRect.Min = FIntPoint::ZeroValue;
	}

	/** Returns the texture resolution that will be output. */
	FIntPoint GetOutputExtent() const;

	/** Validate the settings of TAA, to make sure there is no issue. */
	bool Validate() const;
};

FIntPoint FTAANoBlendPassParameters::GetOutputExtent() const
{
	check(Validate());
	check(SceneColorInput);

	FIntPoint InputExtent = SceneColorInput->Desc.Extent;

	if (!IsTAAUpsamplingConfig(Pass))
		return InputExtent;

	check(OutputViewRect.Min == FIntPoint::ZeroValue);
	FIntPoint PrimaryUpscaleViewSize = FIntPoint::DivideAndRoundUp(OutputViewRect.Size(), ResolutionDivisor);
	FIntPoint QuantizedPrimaryUpscaleViewSize;
	QuantizeSceneBufferSize(PrimaryUpscaleViewSize, QuantizedPrimaryUpscaleViewSize);

	return FIntPoint(
		FMath::Max(InputExtent.X, QuantizedPrimaryUpscaleViewSize.X),
		FMath::Max(InputExtent.Y, QuantizedPrimaryUpscaleViewSize.Y));
}

bool FTAANoBlendPassParameters::Validate() const
{
	if (IsTAAUpsamplingConfig(Pass))
	{
		check(OutputViewRect.Min == FIntPoint::ZeroValue);
	}
	else
	{
		check(InputViewRect == OutputViewRect);
	}
	return true;
}

inline bool DoesPlatformSupportTemporalHistoryUpscale(EShaderPlatform Platform)
{
	return (IsPCPlatform(Platform) || FDataDrivenShaderPlatformInfo::GetSupportsTemporalHistoryUpscale(Platform))
		&& IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
}

class FTAAStandaloneNoBlendCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTAAStandaloneNoBlendCS);
	SHADER_USE_PARAMETER_STRUCT(FTAAStandaloneNoBlendCS, FGlobalShader);

	class FTAAPassConfigDim : SHADER_PERMUTATION_ENUM_CLASS("TAA_PASS_CONFIG", ETAAPassConfig);
	class FTAAFastDim : SHADER_PERMUTATION_BOOL("TAA_FAST");
	class FTAAResponsiveDim : SHADER_PERMUTATION_BOOL("TAA_RESPONSIVE");
	class FTAAScreenPercentageDim : SHADER_PERMUTATION_INT("TAA_SCREEN_PERCENTAGE_RANGE", 4);
	class FTAAUpsampleFilteredDim : SHADER_PERMUTATION_BOOL("TAA_UPSAMPLE_FILTERED");
	class FTAADownsampleDim : SHADER_PERMUTATION_BOOL("TAA_DOWNSAMPLE");

	using FPermutationDomain = TShaderPermutationDomain<
		FTAAPassConfigDim,
		FTAAFastDim,
		FTAAScreenPercentageDim,
		FTAAUpsampleFilteredDim,
		FTAADownsampleDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector4, ViewportUVToInputBufferUV)
		SHADER_PARAMETER(FVector4, MaxViewportUVAndSvPositionToViewportUV)
		SHADER_PARAMETER(FVector2D, ScreenPosAbsMax)
		SHADER_PARAMETER(float, HistoryPreExposureCorrection)
		SHADER_PARAMETER(float, CurrentFrameWeight)
		SHADER_PARAMETER(int32, bCameraCut)

		SHADER_PARAMETER_ARRAY(float, SampleWeights, [9])
		SHADER_PARAMETER_ARRAY(float, PlusWeights, [5])

		SHADER_PARAMETER(FVector4, InputSceneColorSize)
		SHADER_PARAMETER(FIntPoint, InputMinPixelCoord)
		SHADER_PARAMETER(FIntPoint, InputMaxPixelCoord)
		SHADER_PARAMETER(FVector4, OutputViewportSize)
		SHADER_PARAMETER(FVector4, OutputViewportRect)
		SHADER_PARAMETER(FVector, OutputQuantizationError)

		// History parameters
		SHADER_PARAMETER(FVector4, HistoryBufferSize)
		SHADER_PARAMETER(FVector4, HistoryBufferUVMinMax)
		SHADER_PARAMETER(FVector4, ScreenPosToHistoryBufferUV)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EyeAdaptationTexture)
		SHADER_PARAMETER_SRV(Buffer<float4>, EyeAdaptationBuffer)

		// Inputs
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSceneColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneMetadata)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSceneMetadataSampler)

		// History resources
		SHADER_PARAMETER_RDG_TEXTURE_ARRAY(Texture2D, HistoryBuffer, [FTemporalAAHistory::kRenderTargetCount])
		SHADER_PARAMETER_SAMPLER_ARRAY(SamplerState, HistoryBufferSampler, [FTemporalAAHistory::kRenderTargetCount])

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneDepthTextureSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferVelocityTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, GBufferVelocityTextureSampler)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, StencilTexture)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		// Temporal upsample specific parameters.
		SHADER_PARAMETER(FVector4, InputViewSize)
		SHADER_PARAMETER(FVector2D, InputViewMin)
		SHADER_PARAMETER(FVector2D, TemporalJitterPixels)
		SHADER_PARAMETER(float, ScreenPercentage)
		SHADER_PARAMETER(float, UpscaleFactor)

		SHADER_PARAMETER_RDG_TEXTURE_UAV_ARRAY(Texture2D, OutComputeTex, [FTemporalAAHistory::kRenderTargetCount])
		SHADER_PARAMETER_RDG_TEXTURE_UAV(Texture2D, OutComputeTexDownsampled)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, DebugOutput)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		// Screen percentage dimension is only for upsampling permutation.
		if (!IsTAAUpsamplingConfig(PermutationVector.Get<FTAAPassConfigDim>()) &&
			PermutationVector.Get<FTAAScreenPercentageDim>() != 0)
		{
			return false;
		}

		if (PermutationVector.Get<FTAAPassConfigDim>() == ETAAPassConfig::MainSuperSampling)
		{
			// Super sampling is only available in certain configurations.
			if (!DoesPlatformSupportTemporalHistoryUpscale(Parameters.Platform))
			{
				return false;
			}

			// No point disabling filtering.
			if (!PermutationVector.Get<FTAAUpsampleFilteredDim>())
			{
				return false;
			}

			// No point doing a fast permutation since it is PC only.
			if (PermutationVector.Get<FTAAFastDim>())
			{
				return false;
			}
		}

		// No point disabling filtering if not using the fast permutation already.
		if (!PermutationVector.Get<FTAAUpsampleFilteredDim>() &&
			!PermutationVector.Get<FTAAFastDim>())
		{
			return false;
		}

		// No point downsampling if not using the fast permutation already.
		if (PermutationVector.Get<FTAADownsampleDim>() &&
			!PermutationVector.Get<FTAAFastDim>())
		{
			return false;
		}

		// Screen percentage range 3 is only for super sampling.
		if (PermutationVector.Get<FTAAPassConfigDim>() != ETAAPassConfig::MainSuperSampling &&
			PermutationVector.Get<FTAAScreenPercentageDim>() == 3)
		{
			return false;
		}

		// Fast dimensions is only for Main and Diaphragm DOF.
		if (PermutationVector.Get<FTAAFastDim>() &&
			!IsMainTAAConfig(PermutationVector.Get<FTAAPassConfigDim>()) &&
			!IsDOFTAAConfig(PermutationVector.Get<FTAAPassConfigDim>()))
		{
			return false;
		}

		// Non filtering option is only for upsampling.
		if (!PermutationVector.Get<FTAAUpsampleFilteredDim>() &&
			PermutationVector.Get<FTAAPassConfigDim>() != ETAAPassConfig::MainUpsampling)
		{
			return false;
		}

		// TAA_DOWNSAMPLE is only only for Main and MainUpsampling configs.
		if (PermutationVector.Get<FTAADownsampleDim>() &&
			!IsMainTAAConfig(PermutationVector.Get<FTAAPassConfigDim>()))
		{
			return false;
		}

		//Only Main and MainUpsampling config without DownSample permutations are supported on mobile platform.
		return SupportsGen4TAA(Parameters.Platform) && (!IsMobilePlatform(Parameters.Platform) || ((PermutationVector.Get<FTAAPassConfigDim>() == ETAAPassConfig::Main || PermutationVector.Get<FTAAPassConfigDim>() == ETAAPassConfig::MainUpsampling) && !PermutationVector.Get<FTAADownsampleDim>()));
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEX"), GTemporalAATileSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZEY"), GTemporalAATileSizeY);

		bool bIsMobileTiledGPU = RHIHasTiledGPU(Parameters.Platform) || IsSimulatedPlatform(Parameters.Platform);

		// There are some mobile specific shader optimizations need to be set in the shader, such as disable shared memory usage, disable stencil texture sampling.
		OutEnvironment.SetDefine(TEXT("AA_MOBILE_CONFIG"), bIsMobileTiledGPU ? 1 : 0);
	}
};

IMPLEMENT_GLOBAL_SHADER(FTAAStandaloneNoBlendCS, "/Plugin/XeSSCapture/Private/TAAStandalone_noblend.usf", "MainCS", SF_Compute);

static bool XeSSCapture_IsPostProcessingWithAlphaChannelSupported()
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.PostProcessing.PropagateAlpha"));
	return CVar->GetValueOnAnyThread() != 0;
}

static EPostProcessAAQuality XeSSCapture_GetPostProcessAAQuality()
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.PostProcessAAQuality"));

	return static_cast<EPostProcessAAQuality>(FMath::Clamp(CVar->GetValueOnAnyThread(), 0, static_cast<int32>(EPostProcessAAQuality::MAX) - 1));
}

static float CatmullRom(float x)
{
	float ax = FMath::Abs(x);
	if (ax > 1.0f)
		return ((-0.5f * ax + 2.5f) * ax - 4.0f) * ax + 2.0f;
	else
		return (1.5f * ax - 2.5f) * ax * ax + 1.0f;
}

static FVector ComputePixelFormatQuantizationError(EPixelFormat PixelFormat)
{
	FVector Error;
	if (PixelFormat == PF_FloatRGBA || PixelFormat == PF_FloatR11G11B10)
	{
		FIntVector HistoryColorMantissaBits = PixelFormat == PF_FloatR11G11B10 ? FIntVector(6, 6, 5) : FIntVector(10, 10, 10);

		Error.X = FMath::Pow(0.5f, HistoryColorMantissaBits.X);
		Error.Y = FMath::Pow(0.5f, HistoryColorMantissaBits.Y);
		Error.Z = FMath::Pow(0.5f, HistoryColorMantissaBits.Z);
	}
	else
	{
		check(0);
	}

	return Error;
}

static void SetupSampleWeightParameters(FTAAStandaloneNoBlendCS::FParameters* OutTAAParameters, const FTAANoBlendPassParameters& PassParameters, FVector2D TemporalJitterPixels)
{
	float JitterX = TemporalJitterPixels.X;
	float JitterY = TemporalJitterPixels.Y;
	float ResDivisorInv = 1.0f / float(PassParameters.ResolutionDivisor);

	static const float SampleOffsets[9][2] =
	{
		{ -1.0f, -1.0f },
		{  0.0f, -1.0f },
		{  1.0f, -1.0f },
		{ -1.0f,  0.0f },
		{  0.0f,  0.0f },
		{  1.0f,  0.0f },
		{ -1.0f,  1.0f },
		{  0.0f,  1.0f },
		{  1.0f,  1.0f },
	};

	static const auto TemporalAAFilterSize = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.TemporalAAFilterSize"));
	float FilterSize = TemporalAAFilterSize->GetValueOnAnyThread();
	static const auto TemporalAACatmullRom = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.TemporalAACatmullRom"));
	int32 bCatmullRom = TemporalAACatmullRom->GetValueOnAnyThread();

	// Compute 3x3 weights
	{
		float TotalWeight = 0.0f;
		for (int32 i = 0; i < 9; i++)
		{
			float PixelOffsetX = SampleOffsets[i][0] - JitterX * ResDivisorInv;
			float PixelOffsetY = SampleOffsets[i][1] - JitterY * ResDivisorInv;

			PixelOffsetX /= FilterSize;
			PixelOffsetY /= FilterSize;

			if (bCatmullRom)
			{
				OutTAAParameters->SampleWeights[i] = CatmullRom(PixelOffsetX) * CatmullRom(PixelOffsetY);
				TotalWeight += OutTAAParameters->SampleWeights[i];
			}
			else
			{
				// Normal distribution, Sigma = 0.47
				OutTAAParameters->SampleWeights[i] = FMath::Exp(-2.29f * (PixelOffsetX * PixelOffsetX + PixelOffsetY * PixelOffsetY));
				TotalWeight += OutTAAParameters->SampleWeights[i];
			}
		}

		for (int32 i = 0; i < 9; i++)
			OutTAAParameters->SampleWeights[i] /= TotalWeight;
	}

	// Compute 3x3 + weights.
	{
		OutTAAParameters->PlusWeights[0] = OutTAAParameters->SampleWeights[1];
		OutTAAParameters->PlusWeights[1] = OutTAAParameters->SampleWeights[3];
		OutTAAParameters->PlusWeights[2] = OutTAAParameters->SampleWeights[4];
		OutTAAParameters->PlusWeights[3] = OutTAAParameters->SampleWeights[5];
		OutTAAParameters->PlusWeights[4] = OutTAAParameters->SampleWeights[7];
		float TotalWeightPlus = (
			OutTAAParameters->SampleWeights[1] +
			OutTAAParameters->SampleWeights[3] +
			OutTAAParameters->SampleWeights[4] +
			OutTAAParameters->SampleWeights[5] +
			OutTAAParameters->SampleWeights[7]);

		for (int32 i = 0; i < 5; i++)
			OutTAAParameters->PlusWeights[i] /= TotalWeightPlus;
	}
}

static float GetTemporalAAHistoryUpscaleFactor(const FViewInfo& View)
{
	float UpscaleFactor = 1.0f;

	// We only support history upscale in certain configurations.
	if (DoesPlatformSupportTemporalHistoryUpscale(View.GetShaderPlatform()))
	{
		static const auto TemporalAAHistoryScreenPercentage = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.TemporalAA.HistoryScreenPercentage"));
		UpscaleFactor = FMath::Clamp(TemporalAAHistoryScreenPercentage->GetValueOnAnyThread() / 100.0f, 1.0f, 2.0f);
	}

	return UpscaleFactor;
}

// taa pass for sr
static FTAAOutputs AddSrTemporalAAPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FTAANoBlendPassParameters& Inputs,
	const FTemporalAAHistory& InputHistory,
	FTemporalAAHistory* OutputHistory)
{
	check(Inputs.Validate());

	// Whether alpha channel is supported.
	const bool bSupportsAlpha = XeSSCapture_IsPostProcessingWithAlphaChannelSupported();

	// Number of render target in TAA history.
	const int32 IntputTextureCount = (IsDOFTAAConfig(Inputs.Pass) && bSupportsAlpha) ? 2 : 1;

	// Whether this is main TAA pass;
	const bool bIsMainPass = IsMainTAAConfig(Inputs.Pass);

	// Whether to use camera cut shader permutation or not.
	const bool bCameraCut = !InputHistory.IsValid() || View.bCameraCut;

	const FIntPoint OutputExtent = Inputs.GetOutputExtent();

	// Src rectangle.
	const FIntRect SrcRect = Inputs.InputViewRect;
	const FIntRect DestRect = Inputs.OutputViewRect;
	const FIntRect PracticableSrcRect = FIntRect::DivideAndRoundUp(SrcRect, Inputs.ResolutionDivisor);
	const FIntRect PracticableDestRect = FIntRect::DivideAndRoundUp(DestRect, Inputs.ResolutionDivisor);

	const uint32 PassIndex = static_cast<uint32>(Inputs.Pass);

	// Name of the pass.
	const TCHAR* PassName = kTAAPassNames[PassIndex];

	// Create outputs
	FTAAOutputs Outputs;

	TStaticArray<FRDGTextureRef, FTemporalAAHistory::kRenderTargetCount> NewHistoryTexture;

	{
		EPixelFormat HistoryPixelFormat = PF_FloatRGBA;
		static const auto TemporalAAR11G11B10History = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.TemporalAA.R11G11B10History"));
		if (bIsMainPass && Inputs.bUseFast && !bSupportsAlpha && TemporalAAR11G11B10History->GetValueOnAnyThread())
		{
			HistoryPixelFormat = PF_FloatR11G11B10;
		}

		FRDGTextureDesc SceneColorDesc = FRDGTextureDesc::Create2D(
			OutputExtent,
			HistoryPixelFormat,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);

		if (Inputs.bOutputRenderTargetable)
		{
			SceneColorDesc.Flags |= TexCreate_RenderTargetable;
		}

		const TCHAR* OutputName = kTAAOutputNames[PassIndex];

		for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
		{
			NewHistoryTexture[i] = GraphBuilder.CreateTexture(
				SceneColorDesc,
				OutputName,
				ERDGTextureFlags::MultiFrame);
		}

		NewHistoryTexture[0] = Outputs.SceneColor = NewHistoryTexture[0];

		if (IntputTextureCount == 2)
		{
			Outputs.SceneMetadata = NewHistoryTexture[1];
		}

#if CAPTURE_SR_SKIP_CODE
		// should not have down sample here
		check(!Inputs.bDownsample);
#else
		if (Inputs.bDownsample)
		{
			const FRDGTextureDesc HalfResSceneColorDesc = FRDGTextureDesc::Create2D(
				SceneColorDesc.Extent / 2,
				Inputs.DownsampleOverrideFormat != PF_Unknown ? Inputs.DownsampleOverrideFormat : Inputs.SceneColorInput->Desc.Format,
				FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV | GFastVRamConfig.Downsample);

			Outputs.DownsampledSceneColor = GraphBuilder.CreateTexture(HalfResSceneColorDesc, TEXT("SceneColorHalfRes"));
		}
#endif
	}

	TStaticArray<bool, FTemporalAAHistory::kRenderTargetCount> bUseHistoryTexture;

	{
		FTAAStandaloneNoBlendCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAAPassConfigDim>(Inputs.Pass);
		PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAAFastDim>(Inputs.bUseFast);
		PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAADownsampleDim>(Inputs.bDownsample);
		PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAAUpsampleFilteredDim>(true);

		if (IsTAAUpsamplingConfig(Inputs.Pass))
		{
			static const auto TemporalAAUpsampleFiltered = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.TemporalAAUpsampleFiltered"));
			const bool bUpsampleFiltered = TemporalAAUpsampleFiltered->GetValueOnAnyThread() != 0 || Inputs.Pass != ETAAPassConfig::MainUpsampling;
			PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAAUpsampleFilteredDim>(bUpsampleFiltered);

			// If screen percentage > 100% on X or Y axes, then use screen percentage range = 2 shader permutation to disable LDS caching.
			if (SrcRect.Width() > DestRect.Width() ||
				SrcRect.Height() > DestRect.Height())
			{
				PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAAScreenPercentageDim>(2);
			}
			// If screen percentage < 50% on X and Y axes, then use screen percentage range = 3 shader permutation.
			else if (SrcRect.Width() * 100 < 50 * DestRect.Width() &&
				SrcRect.Height() * 100 < 50 * DestRect.Height() &&
				Inputs.Pass == ETAAPassConfig::MainSuperSampling)
			{
				PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAAScreenPercentageDim>(3);
			}
			// If screen percentage < 71% on X and Y axes, then use screen percentage range = 1 shader permutation to have smaller LDS caching.
			else if (SrcRect.Width() * 100 < 71 * DestRect.Width() &&
				SrcRect.Height() * 100 < 71 * DestRect.Height())
			{
				PermutationVector.Set<FTAAStandaloneNoBlendCS::FTAAScreenPercentageDim>(1);
			}
		}

		FTAAStandaloneNoBlendCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTAAStandaloneNoBlendCS::FParameters>();

		// Setups common shader parameters
		const FIntPoint InputExtent = Inputs.SceneColorInput->Desc.Extent;
		const FIntRect InputViewRect = Inputs.InputViewRect;
		const FIntRect OutputViewRect = Inputs.OutputViewRect;

		if (!IsTAAUpsamplingConfig(Inputs.Pass))
		{
			SetupSampleWeightParameters(PassParameters, Inputs, View.TemporalJitterPixels);
		}

		const float ResDivisor = Inputs.ResolutionDivisor;
		const float ResDivisorInv = 1.0f / ResDivisor;

		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
		static const auto TemporalAACurrentFrameWeight = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.TemporalAACurrentFrameWeight"));
		PassParameters->CurrentFrameWeight = TemporalAACurrentFrameWeight->GetValueOnAnyThread();
		PassParameters->bCameraCut = bCameraCut;

		PassParameters->SceneDepthTexture = Inputs.SceneDepthTexture;
		PassParameters->GBufferVelocityTexture = Inputs.SceneVelocityTexture;

		PassParameters->SceneDepthTextureSampler = TStaticSamplerState<SF_Point>::GetRHI();
		PassParameters->GBufferVelocityTextureSampler = TStaticSamplerState<SF_Point>::GetRHI();

		PassParameters->StencilTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(Inputs.SceneDepthTexture, PF_X24_G8));

		// We need a valid velocity buffer texture. Use black (no velocity) if none exists.
		if (!PassParameters->GBufferVelocityTexture)
		{
			PassParameters->GBufferVelocityTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);;
		}

		// Input buffer shader parameters
		{
			PassParameters->InputSceneColorSize = FVector4(
				InputExtent.X,
				InputExtent.Y,
				1.0f / float(InputExtent.X),
				1.0f / float(InputExtent.Y));
			PassParameters->InputMinPixelCoord = PracticableSrcRect.Min;
			PassParameters->InputMaxPixelCoord = PracticableSrcRect.Max - FIntPoint(1, 1);
			PassParameters->InputSceneColor = Inputs.SceneColorInput;
			PassParameters->InputSceneColorSampler = TStaticSamplerState<SF_Point>::GetRHI();
			PassParameters->InputSceneMetadata = Inputs.SceneMetadataInput;
			PassParameters->InputSceneMetadataSampler = TStaticSamplerState<SF_Point>::GetRHI();
		}

		PassParameters->OutputViewportSize = FVector4(
			PracticableDestRect.Width(), PracticableDestRect.Height(), 1.0f / float(PracticableDestRect.Width()), 1.0f / float(PracticableDestRect.Height()));
		PassParameters->OutputViewportRect = FVector4(PracticableDestRect.Min.X, PracticableDestRect.Min.Y, PracticableDestRect.Max.X, PracticableDestRect.Max.Y);
		PassParameters->OutputQuantizationError = ComputePixelFormatQuantizationError(NewHistoryTexture[0]->Desc.Format);

		// Set history shader parameters.
		{
			FRDGTextureRef BlackDummy = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy);

			if (bCameraCut)
			{
				PassParameters->ScreenPosToHistoryBufferUV = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
				PassParameters->ScreenPosAbsMax = FVector2D(0.0f, 0.0f);
				PassParameters->HistoryBufferUVMinMax = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
				PassParameters->HistoryBufferSize = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

				for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
				{
					PassParameters->HistoryBuffer[i] = BlackDummy;
				}

				// Remove dependency of the velocity buffer on camera cut, given it's going to be ignored by the shader.
				PassParameters->GBufferVelocityTexture = BlackDummy;
			}
			else
			{
				FIntPoint ReferenceViewportOffset = InputHistory.ViewportRect.Min;
				FIntPoint ReferenceViewportExtent = InputHistory.ViewportRect.Size();
				FIntPoint ReferenceBufferSize = InputHistory.ReferenceBufferSize;

				float InvReferenceBufferSizeX = 1.f / float(InputHistory.ReferenceBufferSize.X);
				float InvReferenceBufferSizeY = 1.f / float(InputHistory.ReferenceBufferSize.Y);

				PassParameters->ScreenPosToHistoryBufferUV = FVector4(
					ReferenceViewportExtent.X * 0.5f * InvReferenceBufferSizeX,
					-ReferenceViewportExtent.Y * 0.5f * InvReferenceBufferSizeY,
					(ReferenceViewportExtent.X * 0.5f + ReferenceViewportOffset.X) * InvReferenceBufferSizeX,
					(ReferenceViewportExtent.Y * 0.5f + ReferenceViewportOffset.Y) * InvReferenceBufferSizeY);

				FIntPoint ViewportOffset = ReferenceViewportOffset / Inputs.ResolutionDivisor;
				FIntPoint ViewportExtent = FIntPoint::DivideAndRoundUp(ReferenceViewportExtent, Inputs.ResolutionDivisor);
				FIntPoint BufferSize = ReferenceBufferSize / Inputs.ResolutionDivisor;

				PassParameters->ScreenPosAbsMax = FVector2D(1.0f - 1.0f / float(ViewportExtent.X), 1.0f - 1.0f / float(ViewportExtent.Y));

				float InvBufferSizeX = 1.f / float(BufferSize.X);
				float InvBufferSizeY = 1.f / float(BufferSize.Y);

				PassParameters->HistoryBufferUVMinMax = FVector4(
					(ViewportOffset.X + 0.5f) * InvBufferSizeX,
					(ViewportOffset.Y + 0.5f) * InvBufferSizeY,
					(ViewportOffset.X + ViewportExtent.X - 0.5f) * InvBufferSizeX,
					(ViewportOffset.Y + ViewportExtent.Y - 0.5f) * InvBufferSizeY);

				PassParameters->HistoryBufferSize = FVector4(BufferSize.X, BufferSize.Y, InvBufferSizeX, InvBufferSizeY);

				for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
				{
					if (InputHistory.RT[i].IsValid())
					{
						PassParameters->HistoryBuffer[i] = GraphBuilder.RegisterExternalTexture(InputHistory.RT[i]);
					}
					else
					{
						PassParameters->HistoryBuffer[i] = BlackDummy;
					}
				}
			}

			for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
			{
				PassParameters->HistoryBufferSampler[i] = TStaticSamplerState<SF_Bilinear>::GetRHI();
			}
		}

		PassParameters->MaxViewportUVAndSvPositionToViewportUV = FVector4(
			(PracticableDestRect.Width() - 0.5f * ResDivisor) / float(PracticableDestRect.Width()),
			(PracticableDestRect.Height() - 0.5f * ResDivisor) / float(PracticableDestRect.Height()),
			ResDivisor / float(DestRect.Width()),
			ResDivisor / float(DestRect.Height()));

		PassParameters->HistoryPreExposureCorrection = View.PreExposure / View.PrevViewInfo.SceneColorPreExposure;

		{
			float InvSizeX = 1.0f / float(InputExtent.X);
			float InvSizeY = 1.0f / float(InputExtent.Y);
			PassParameters->ViewportUVToInputBufferUV = FVector4(
				ResDivisorInv * InputViewRect.Width() * InvSizeX,
				ResDivisorInv * InputViewRect.Height() * InvSizeY,
				ResDivisorInv * InputViewRect.Min.X * InvSizeX,
				ResDivisorInv * InputViewRect.Min.Y * InvSizeY);
		}

		if (View.GetFeatureLevel() <= ERHIFeatureLevel::ES3_1)
		{
			PassParameters->EyeAdaptationBuffer = GetEyeAdaptationBuffer(View);
		}
		else
		{
			PassParameters->EyeAdaptationTexture = GetEyeAdaptationTexture(GraphBuilder, View);
		}

		// Temporal upsample specific shader parameters.
		{
			// Temporal AA upscale specific params.
			float InputViewSizeInvScale = Inputs.ResolutionDivisor;
			float InputViewSizeScale = 1.0f / InputViewSizeInvScale;

			PassParameters->TemporalJitterPixels = InputViewSizeScale * View.TemporalJitterPixels;
			PassParameters->ScreenPercentage = float(InputViewRect.Width()) / float(OutputViewRect.Width());
			PassParameters->UpscaleFactor = float(OutputViewRect.Width()) / float(InputViewRect.Width());
			PassParameters->InputViewMin = InputViewSizeScale * FVector2D(InputViewRect.Min.X, InputViewRect.Min.Y);
			PassParameters->InputViewSize = FVector4(
				InputViewSizeScale * InputViewRect.Width(), InputViewSizeScale * InputViewRect.Height(),
				InputViewSizeInvScale / InputViewRect.Width(), InputViewSizeInvScale / InputViewRect.Height());
		}

		// UAVs
		{
			for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
			{
				PassParameters->OutComputeTex[i] = GraphBuilder.CreateUAV(NewHistoryTexture[i]);
			}

			if (Outputs.DownsampledSceneColor)
			{
				PassParameters->OutComputeTexDownsampled = GraphBuilder.CreateUAV(Outputs.DownsampledSceneColor);
			}
		}

		// Debug UAVs
		{
			FRDGTextureDesc DebugDesc = FRDGTextureDesc::Create2D(
				OutputExtent,
				PF_FloatRGBA,
				FClearValueBinding::None,
				/* InFlags = */ TexCreate_ShaderResource | TexCreate_UAV);

			FRDGTextureRef DebugTexture = GraphBuilder.CreateTexture(DebugDesc, TEXT("Debug.TAA"));
			PassParameters->DebugOutput = GraphBuilder.CreateUAV(DebugTexture);
		}

		TShaderMapRef<FTAAStandaloneNoBlendCS> ComputeShader(View.ShaderMap, PermutationVector);

		ClearUnusedGraphResources(ComputeShader, PassParameters);
		for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
		{
			bUseHistoryTexture[i] = PassParameters->HistoryBuffer[i] != nullptr;
		}

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TAA No Blend %s%s %dx%d -> %dx%d",
				PassName, Inputs.bUseFast ? TEXT(" Fast") : TEXT(""),
				PracticableSrcRect.Width(), PracticableSrcRect.Height(),
				PracticableDestRect.Width(), PracticableDestRect.Height()),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX));
	}

	if (!View.bStatePrevViewInfoIsReadOnly)
	{
		OutputHistory->SafeRelease();

		for (int32 i = 0; i < FTemporalAAHistory::kRenderTargetCount; i++)
		{
			if (bUseHistoryTexture[i])
			{
				GraphBuilder.QueueTextureExtraction(NewHistoryTexture[i], &OutputHistory->RT[i]);
			}
		}

		OutputHistory->ViewportRect = DestRect;
		OutputHistory->ReferenceBufferSize = OutputExtent * Inputs.ResolutionDivisor;
	}

	return Outputs;
}

// taa pass for sr
static void AddSrMainTemporalAAPasses(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const ITemporalUpscaler::FPassInputs& PassInputs,
	FRDGTextureRef* OutSceneColorTexture,
	FIntRect* OutSceneColorViewRect,
	FRDGTextureRef* OutSceneColorHalfResTexture,
	FIntRect* OutSceneColorHalfResViewRect)
{
	check(View.AntiAliasingMethod == AAM_TemporalAA && View.ViewState);

	FTAANoBlendPassParameters TAAParameters(View);

	TAAParameters.Pass = View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale
		? ETAAPassConfig::MainUpsampling
		: ETAAPassConfig::Main;

	TAAParameters.SetupViewRect(View);

	const EPostProcessAAQuality LowQualityTemporalAA = EPostProcessAAQuality::Medium;

	TAAParameters.bUseFast = XeSSCapture_GetPostProcessAAQuality() == LowQualityTemporalAA;

	const FIntRect SecondaryViewRect = TAAParameters.OutputViewRect;

	const float HistoryUpscaleFactor = GetTemporalAAHistoryUpscaleFactor(View);

#if CAPTURE_SR_SKIP_CODE
	check(!(HistoryUpscaleFactor > 1.0f));
#else
	// Configures TAA to upscale the history buffer; this is in addition to the secondary screen percentage upscale.
	// We end up with a scene color that is larger than the secondary screen percentage. We immediately downscale
	// afterwards using a Mitchel-Netravali filter.
	if (HistoryUpscaleFactor > 1.0f)
	{
		const FIntPoint HistoryViewSize(
			TAAParameters.OutputViewRect.Width() * HistoryUpscaleFactor,
			TAAParameters.OutputViewRect.Height() * HistoryUpscaleFactor);

		TAAParameters.Pass = ETAAPassConfig::MainSuperSampling;
		TAAParameters.bUseFast = false;

		TAAParameters.OutputViewRect.Min.X = 0;
		TAAParameters.OutputViewRect.Min.Y = 0;
		TAAParameters.OutputViewRect.Max = HistoryViewSize;
	}
#endif

	TAAParameters.DownsampleOverrideFormat = PassInputs.DownsampleOverrideFormat;

	TAAParameters.bDownsample = PassInputs.bAllowDownsampleSceneColor && TAAParameters.bUseFast;

	TAAParameters.SceneDepthTexture = PassInputs.SceneDepthTexture;
	TAAParameters.SceneVelocityTexture = PassInputs.SceneVelocityTexture;
	TAAParameters.SceneColorInput = PassInputs.SceneColorTexture;

	const FTemporalAAHistory& InputHistory = View.PrevViewInfo.TemporalAAHistory;

	FTemporalAAHistory& OutputHistory = View.ViewState->PrevFrameViewInfo.TemporalAAHistory;

	const FTAAOutputs TAAOutputs = AddSrTemporalAAPass(
		GraphBuilder,
		View,
		TAAParameters,
		InputHistory,
		&OutputHistory);

	FRDGTextureRef SceneColorTexture = TAAOutputs.SceneColor;

#if CAPTURE_SR_SKIP_CODE
	check(!(HistoryUpscaleFactor > 1.0f));
#else
	// If we upscaled the history buffer, downsize back to the secondary screen percentage size.
	if (HistoryUpscaleFactor > 1.0f)
	{
		const FIntRect InputViewport = TAAParameters.OutputViewRect;

		FIntPoint QuantizedOutputSize;
		QuantizeSceneBufferSize(SecondaryViewRect.Size(), QuantizedOutputSize);

		FScreenPassTextureViewport OutputViewport;
		OutputViewport.Rect = SecondaryViewRect;
		OutputViewport.Extent.X = FMath::Max(PassInputs.SceneColorTexture->Desc.Extent.X, QuantizedOutputSize.X);
		OutputViewport.Extent.Y = FMath::Max(PassInputs.SceneColorTexture->Desc.Extent.Y, QuantizedOutputSize.Y);

		SceneColorTexture = ComputeMitchellNetravaliDownsample(GraphBuilder, View, FScreenPassTexture(SceneColorTexture, InputViewport), OutputViewport);
	}
#endif

	*OutSceneColorTexture = SceneColorTexture;
	*OutSceneColorViewRect = SecondaryViewRect;
	*OutSceneColorHalfResTexture = TAAOutputs.DownsampledSceneColor;
	*OutSceneColorHalfResViewRect = FIntRect::DivideAndRoundUp(SecondaryViewRect, 2);
}

#if ENGINE_MAJOR_VERSION < 5
void FXeSSCaptureTemporalUpscaler::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FPassInputs& PassInputs,
	FRDGTextureRef* OutSceneColorTexture,
	FIntRect* OutSceneColorViewRect,
	FRDGTextureRef* OutSceneColorHalfResTexture,
	FIntRect* OutSceneColorHalfResViewRect) const
#else
ITemporalUpscaler::FOutputs FXeSSVelocityTemporalUpscaler::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FPassInputs& PassInputs) const
#endif
{
	FIntRect FirstViewRect;
	FirstViewRect.Min = FIntPoint(0, 0);
	FirstViewRect.Max = View.ViewRect.Size();
	FIntRect SecondaryViewRect;
	SecondaryViewRect.Min = FIntPoint(0, 0);
	SecondaryViewRect.Max = View.GetSecondaryViewRectSize();

	static const auto OutputDirectory = IConsoleManager::Get().FindConsoleVariable(TEXT("r.XeSSCapture.OutputDirectory"));
	static const auto DateTime = IConsoleManager::Get().FindConsoleVariable(TEXT("r.XeSSCapture.DateTime"));
	static const auto FrameIndex = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.XeSSCapture.FrameIndex"));

	// color
	FScreenPassTexture SceneColor(PassInputs.SceneColorTexture, FirstViewRect);
	if (FrameIndex->GetValueOnRenderThread() >= 0)
	{
		AddDumpToExrPass(GraphBuilder, SceneColor, FString::Printf(TEXT("%s\\%s\\color\\color_%d.exr"), *OutputDirectory->GetString(), *DateTime->GetString(), FrameIndex->GetValueOnRenderThread()));
	}

	// sampler
	const FRDGTextureRef XeSSSampler = AddSamplePass(GraphBuilder, View);
	if (FrameIndex->GetValueOnRenderThread() >= 0)
	{
		FScreenPassTexture SceneSampler(XeSSSampler, FirstViewRect);
		AddDumpToExrPass(GraphBuilder, SceneSampler, FString::Printf(TEXT("%s\\%s\\sample\\sample_%d.exr"), *OutputDirectory->GetString(), *DateTime->GetString(), FrameIndex->GetValueOnRenderThread()));
	}

	// depth
	const FRDGTextureRef XeSSDepth = AddDepthXeSSPass(GraphBuilder, PassInputs.SceneDepthTexture, View);
	if (FrameIndex->GetValueOnRenderThread() >= 0)
	{
		FScreenPassTexture SceneDepth(XeSSDepth, FirstViewRect);
		AddDumpToExrPass(GraphBuilder, SceneDepth, FString::Printf(TEXT("%s\\%s\\depth\\depth_%d.exr"), *OutputDirectory->GetString(), *DateTime->GetString(), FrameIndex->GetValueOnRenderThread()));
	}

	// low-res velocity
	const FRDGTextureRef XeSSLowResVelocity = AddLowResVelocityXeSSPass(GraphBuilder,
		PassInputs.SceneDepthTexture,
		PassInputs.SceneVelocityTexture,
		View);
	if (FrameIndex->GetValueOnRenderThread() >= 0)
	{
		FScreenPassTexture SceneLowResVelocity(XeSSLowResVelocity, FirstViewRect);
		AddDumpToExrPass(GraphBuilder, SceneLowResVelocity, FString::Printf(TEXT("%s\\%s\\lowres_velocity\\lowres_velocity_%d.exr"), *OutputDirectory->GetString(), *DateTime->GetString(), FrameIndex->GetValueOnRenderThread()));
	}

	// velocity
	const FRDGTextureRef XeSSUpscaledVelocity = AddVelocityFlatteningXeSSPass(GraphBuilder,
		PassInputs.SceneDepthTexture,
		PassInputs.SceneVelocityTexture,
		View);
	if (FrameIndex->GetValueOnRenderThread() >= 0)
	{
		FScreenPassTexture SceneVelocity(XeSSUpscaledVelocity, SecondaryViewRect);
		AddDumpToExrPass(GraphBuilder, SceneVelocity, FString::Printf(TEXT("%s\\%s\\velocity\\velocity_%d.exr"), *OutputDirectory->GetString(), *DateTime->GetString(), FrameIndex->GetValueOnRenderThread()));
	}

#if ENGINE_MAJOR_VERSION < 5
	ITemporalUpscaler::GetDefaultTemporalUpscaler()->AddPasses(GraphBuilder, View, PassInputs, OutSceneColorTexture, OutSceneColorViewRect, OutSceneColorHalfResTexture, OutSceneColorHalfResViewRect);
#else
	return ITemporalUpscaler::GetDefaultTemporalUpscaler()->AddPasses(GraphBuilder, View, PassInputs);
#endif
}

static void SaveSampleToJson(const FString& fileName, float sampleX, float sampleY)
{
	TSharedRef<FJsonObject> RootObject = MakeShareable(new FJsonObject);

	RootObject->SetNumberField(TEXT("jitterX"), sampleX);
	RootObject->SetNumberField(TEXT("jitterY"), sampleY);

	//Write the json file
	FString Json;
	TSharedRef<TJsonWriter<> > JsonWriter = TJsonWriterFactory<>::Create(&Json, 0);
	if (FJsonSerializer::Serialize(RootObject, JsonWriter))
	{
		FFileHelper::SaveStringToFile(Json, *fileName);
	}
}

#if ENGINE_MAJOR_VERSION < 5
void FXeSSCaptureNoBlenderUpscaler::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FPassInputs& PassInputs,
	FRDGTextureRef* OutSceneColorTexture,
	FIntRect* OutSceneColorViewRect,
	FRDGTextureRef* OutSceneColorHalfResTexture,
	FIntRect* OutSceneColorHalfResViewRect) const
#else
ITemporalUpscaler::FOutputs FXeSSVelocityTemporalUpscaler::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FPassInputs& PassInputs) const
#endif
{
#if ENGINE_MAJOR_VERSION < 5
#else
	ITemporalUpscaler::FOutputs Outputs{};
#endif

	FIntRect SecondaryViewRect;
	SecondaryViewRect.Min = FIntPoint(0, 0);
	SecondaryViewRect.Max = View.GetSecondaryViewRectSize();

	static const auto OutputDirectory = IConsoleManager::Get().FindConsoleVariable(TEXT("r.XeSSCapture.OutputDirectory"));
	static const auto DateTime = IConsoleManager::Get().FindConsoleVariable(TEXT("r.XeSSCapture.DateTime"));
	static const auto FrameIndex = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.XeSSCapture.FrameIndex"));
	static const auto SampleIndex = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.XeSSCapture.SampleIndex"));

	if (FrameIndex->GetValueOnRenderThread() == 0)
	{
		SaveSampleToJson(FString::Printf(TEXT("%s\\%s\\sr\\samples\\srsample_%d.json"), *OutputDirectory->GetString(), *DateTime->GetString(), SampleIndex->GetValueOnRenderThread()), View.TemporalJitterPixels.X, View.TemporalJitterPixels.Y);
	}

	// sr
	AddSrMainTemporalAAPasses(GraphBuilder, View, PassInputs, OutSceneColorTexture, OutSceneColorViewRect, OutSceneColorHalfResTexture, OutSceneColorHalfResViewRect);
	if (FrameIndex->GetValueOnRenderThread() >= 0)
	{
		FScreenPassTexture SceneSr(*OutSceneColorTexture, SecondaryViewRect);
		AddDumpToExrPass(GraphBuilder, SceneSr, FString::Printf(TEXT("%s\\%s\\sr\\%d\\sr_%d.exr"), *OutputDirectory->GetString(), *DateTime->GetString(), FrameIndex->GetValueOnRenderThread(), SampleIndex->GetValueOnRenderThread()));
	}

#if ENGINE_MAJOR_VERSION < 5
#else
	Outputs.FullRes.Texture = *OutSceneColorTexture;
	Outputs.FullRes.ViewRect = SecondaryViewRect;

	return Outputs;
#endif
}
