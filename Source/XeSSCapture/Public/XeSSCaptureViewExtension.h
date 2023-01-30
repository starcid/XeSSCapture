#pragma once

#include "SceneViewExtension.h"

class FXeSSCaptureTemporalUpscaler;
class FXeSSCaptureNoBlenderUpscaler;
class FXeSSCaptureViewExtension final : public FSceneViewExtensionBase
{
public:
	FXeSSCaptureViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) { 
		m_bActive = false;
		m_bCaptureSR = false;
	}

	bool IsActive() { return m_bActive; }
	void SetActive(bool isActive) { m_bActive = isActive; }
	void SetCaptureSR(bool isCaptureSR) { m_bCaptureSR = isCaptureSR; }

	// ISceneViewExtension interface
	void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	void PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) override {}
	void PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily) {}
	
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;

private:
	bool m_bActive;
	bool m_bCaptureSR;
};