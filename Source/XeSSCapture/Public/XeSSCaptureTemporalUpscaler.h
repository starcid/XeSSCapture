#pragma once

#include "CoreMinimal.h"
#include "PostProcess/TemporalAA.h"

class FXeSSCaptureTemporalUpscaler final : public ITemporalUpscaler
{
public:
	FXeSSCaptureTemporalUpscaler() {}
	virtual ~FXeSSCaptureTemporalUpscaler() {}

	virtual const TCHAR* GetDebugName() const
	{
		return TEXT("XeSSCaptureTemporalUpscaler");
	}

#if ENGINE_MAJOR_VERSION < 5
	virtual void AddPasses(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FPassInputs& PassInputs,
		FRDGTextureRef* OutSceneColorTexture,
		FIntRect* OutSceneColorViewRect,
		FRDGTextureRef* OutSceneColorHalfResTexture,
		FIntRect* OutSceneColorHalfResViewRect) const final;
#else
	virtual FOutputs AddPasses(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FPassInputs& PassInputs) const final;
#endif

	virtual float GetMinUpsampleResolutionFraction() const override
	{
		return FSceneViewScreenPercentageConfig::kMinTAAUpsampleResolutionFraction;
	}
	virtual float GetMaxUpsampleResolutionFraction() const override
	{
		return FSceneViewScreenPercentageConfig::kMaxTAAUpsampleResolutionFraction;
	}
};

class FXeSSCaptureNoBlenderUpscaler final : public ITemporalUpscaler
{
public:
	FXeSSCaptureNoBlenderUpscaler() {}
	virtual ~FXeSSCaptureNoBlenderUpscaler() {}

	virtual const TCHAR* GetDebugName() const
	{
		return TEXT("XeSSCaptureNoBlenderUpscaler");
	}

#if ENGINE_MAJOR_VERSION < 5
	virtual void AddPasses(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FPassInputs& PassInputs,
		FRDGTextureRef* OutSceneColorTexture,
		FIntRect* OutSceneColorViewRect,
		FRDGTextureRef* OutSceneColorHalfResTexture,
		FIntRect* OutSceneColorHalfResViewRect) const final;
#else
	virtual FOutputs AddPasses(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FPassInputs& PassInputs) const final;
#endif

	virtual float GetMinUpsampleResolutionFraction() const override
	{
		return FSceneViewScreenPercentageConfig::kMinTAAUpsampleResolutionFraction;
	}
	virtual float GetMaxUpsampleResolutionFraction() const override
	{
		return FSceneViewScreenPercentageConfig::kMaxTAAUpsampleResolutionFraction;
	}
};