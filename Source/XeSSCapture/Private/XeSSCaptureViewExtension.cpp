#include "XeSSCaptureViewExtension.h"
#include "XeSSCaptureTemporalUpscaler.h"

static FXeSSCaptureTemporalUpscaler m_CaptureTemporalUpscaler;
static FXeSSCaptureNoBlenderUpscaler m_CaptureNoBlenderUpscaler;

void FXeSSCaptureViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	if (m_bActive)
	{
		InViewFamily.EngineShowFlags.Particles = 0;
		InViewFamily.EngineShowFlags.Fog = 0;
		InViewFamily.EngineShowFlags.VolumetricFog = 0;
	}
}

void FXeSSCaptureViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	if (m_bActive)
	{
		GTemporalUpscaler = m_bCaptureSR ? (ITemporalUpscaler*)&m_CaptureNoBlenderUpscaler : (ITemporalUpscaler*)&m_CaptureTemporalUpscaler;
		InViewFamily.SetTemporalUpscalerInterface(m_bCaptureSR ? (ITemporalUpscaler*)&m_CaptureNoBlenderUpscaler : (ITemporalUpscaler*)&m_CaptureTemporalUpscaler);
	}
}
