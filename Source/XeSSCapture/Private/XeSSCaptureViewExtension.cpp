#include "XeSSCaptureViewExtension.h"
#include "XeSSCaptureTemporalUpscaler.h"
#include "ScenePrivate.h"

static FXeSSCaptureTemporalUpscaler m_CaptureTemporalUpscaler;
static FXeSSCaptureNoBlenderUpscaler m_CaptureNoBlenderUpscaler;

void FXeSSCaptureViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	FSceneViewState* viewState = static_cast<FSceneViewState*>(InView.State);
	if (viewState == nullptr)
		return;

	// need to reserve TemporalAASampleIndex and FrameIndex in ViewState here
	// where Sample jitters(TemporalAASampleIndex) & StateFrameIndexMod8 are all used in TAA no blend shading

	if (m_bActive)
	{
		if (m_bHaveSetView)
			return;

		if (!m_bCaptureSR)
		{
			// 32 samples
			// if reserved, then restore value
			if (m_iReserveView0FrameIndex >= 0)
			{
				viewState->FrameIndex = m_iReserveView0FrameIndex;
			}
			if (m_iReserveView0TemporalAAIndex >= 0)
			{
				viewState->TemporalAASampleIndex = m_iReserveView0TemporalAAIndex;
			}
			// reserve for next frame
			m_iReserveView0FrameIndex = viewState->FrameIndex + 1;
			m_iReserveView0TemporalAAIndex = viewState->TemporalAASampleIndex + 1;
			if (m_iReserveView0TemporalAAIndex >= 32)
			{
				m_iReserveView0TemporalAAIndex = 0;
			}
		}
		else
		{
			// 256 samples
			static const auto SampleIndex = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.XeSSCapture.SampleIndex"));
			viewState->TemporalAASampleIndex = SampleIndex->GetValueOnGameThread() - 1;
			viewState->FrameIndex = SampleIndex->GetValueOnGameThread() - 1;
		}
		m_bHaveSetView = true;
	}
	else
	{
		// restore for normal renderering
		if (m_iReserveView0FrameIndex >= 0)
		{
			viewState->FrameIndex = m_iReserveView0FrameIndex;
			m_iReserveView0FrameIndex = -1;
		}
		if (m_iReserveView0TemporalAAIndex >= 0)
		{
			viewState->TemporalAASampleIndex = m_iReserveView0TemporalAAIndex;
			m_iReserveView0TemporalAAIndex = -1;
		}
	}
}

void FXeSSCaptureViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	if (m_bActive)
	{
		InViewFamily.EngineShowFlags.Particles = 0;
		InViewFamily.EngineShowFlags.Fog = 0;
		InViewFamily.EngineShowFlags.VolumetricFog = 0;
		m_bHaveSetView = false;
	}
}

void FXeSSCaptureViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	if (m_bActive)
	{
#if ENGINE_MAJOR_VERSION < 5
		GTemporalUpscaler = m_bCaptureSR ? (ITemporalUpscaler*)&m_CaptureNoBlenderUpscaler : (ITemporalUpscaler*)&m_CaptureTemporalUpscaler;
#endif
#if ENGINE_MAJOR_VERSION < 5
		InViewFamily.SetTemporalUpscalerInterface(m_bCaptureSR ? (ITemporalUpscaler*)&m_CaptureNoBlenderUpscaler : (ITemporalUpscaler*)&m_CaptureTemporalUpscaler);
#else
		InViewFamily.SetTemporalUpscalerInterface(m_bCaptureSR ? m_CaptureNoBlenderUpscaler.Fork_GameThread(InViewFamily) : m_CaptureTemporalUpscaler.Fork_GameThread(InViewFamily));
#endif
	}
}
