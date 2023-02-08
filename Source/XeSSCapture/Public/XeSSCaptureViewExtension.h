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
		m_iReserveView0TemporalAAIndex = -1;
		m_iReserveView0FrameIndex = -1;
	}

	bool IsActive() { return m_bActive; }
	void SetActive(bool isActive) { m_bActive = isActive; }
	void SetCaptureSR(bool isCaptureSR) { m_bCaptureSR = isCaptureSR; }

	int GetReserveTemporalAAIndex() { return m_iReserveView0TemporalAAIndex; }

	// ISceneViewExtension interface
	void PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) override {}
	void PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily) {}
	
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;

private:
	bool m_bActive;
	bool m_bCaptureSR;

	int m_iReserveView0TemporalAAIndex;
	int m_iReserveView0FrameIndex;
};