// Copyright Epic Games, Inc. All Rights Reserved.

#include "XeSSCapture.h"

#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"

#include "Engine/Engine.h"
#include "CanvasTypes.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"

#if WITH_EDITOR
#include "Editor.h"
#endif
#include "PostProcess/TemporalAA.h"
#include "XeSSCaptureViewExtension.h"
#include "XeSSCaptureSR.h"

#define LOCTEXT_NAMESPACE "FXeSSCaptureModule"

DEFINE_LOG_CATEGORY(LogXeSSCapture);

static int CaptureFrameCount = 0;
static FAutoConsoleVariableRef CVarXeSSCaptureFrameCount(
	TEXT("r.XeSSCapture"),
	CaptureFrameCount,
	TEXT("XeSS capture start. Last frames"),
	ECVF_RenderThreadSafe);

static float CaptureDelayCount = 0.0f;
static TAutoConsoleVariable<float> CVarXeSSCaptureDelayTime(
	TEXT("r.XeSSCapture.DelayTime"),
	3.0f,
	TEXT("The delay seconds before the capture begin."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarXeSSCaptureFixedFrameRate(
	TEXT("r.XeSSCapture.FixedFrameRate"),
	30.0f,
	TEXT("The fixed frame rate which is used to capture."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarXeSSCaptureScreenPercentage(
	TEXT("r.XeSSCapture.ScreenPercentage"),
	50.0f,
	TEXT("The screen percentage which is used to capture."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<FString> CVarXeSSCaptureOutputDir(
	TEXT("r.XeSSCapture.OutputDirectory"),
	TEXT("D:\\XeSSCapture\\"),
	TEXT("XeSS capture output directory."));

static TAutoConsoleVariable<FString> CVarXeSSCaptureDateTime(
	TEXT("r.XeSSCapture.DateTime"),
	TEXT("NoDateTime"),
	TEXT("The date of this XeSS capture."));

static int32 XeSSCaptureFrameIndex = 0;
static TAutoConsoleVariable<int32> CVarXeSSCaptureFrameIndex(
	TEXT("r.XeSSCapture.FrameIndex"),
	0,
	TEXT("The frame index of this XeSS capture."),
	ECVF_RenderThreadSafe);

void FXeSSCaptureModule::OverrideCommands(FString command, int value)
{
	auto const pVar = IConsoleManager::Get().FindConsoleVariable(*command);
	if (pVar != nullptr)
	{
		if (!m_ReservedCmds.Find(command))
		{
			m_ReservedCmds.Add(command, pVar->GetInt());
			pVar->Set(value);
		}
		else
		{
			UE_LOG(LogXeSSCapture, Warning, TEXT("Command %s has been overrided, value is %d."), *command, m_ReservedCmds[command]);
		}
	}
	else
	{
		UE_LOG(LogXeSSCapture, Warning, TEXT("Override command not found %s."), *command);
	}
}

void FXeSSCaptureModule::RestoreCommands(FString command)
{
	auto const pVar = IConsoleManager::Get().FindConsoleVariable(*command);
	if (pVar != nullptr)
	{
		int* v = m_ReservedCmds.Find(command);
		if (v)
		{
			pVar->Set(*v);
		}
		else
		{
			UE_LOG(LogXeSSCapture, Warning, TEXT("Restore command not set %s."), *command);
		}
	}
	else
	{
		UE_LOG(LogXeSSCapture, Warning, TEXT("Restore command not found %s."), *command);
	}
}

void FXeSSCaptureModule::OverrideCommandsFloat(FString command, float value)
{
	auto const pVar = IConsoleManager::Get().FindConsoleVariable(*command);
	if (pVar != nullptr)
	{
		if (!m_ReservedFloatCmds.Find(command))
		{
			m_ReservedFloatCmds.Add(command, pVar->GetFloat());
			pVar->Set(value);
		}
		else
		{
			UE_LOG(LogXeSSCapture, Warning, TEXT("Command %s has been overrided, value is %d."), *command, m_ReservedFloatCmds[command]);
		}
	}
	else
	{
		UE_LOG(LogXeSSCapture, Warning, TEXT("Override command not found %s."), *command);
	}
}

void FXeSSCaptureModule::RestoreCommandsFloat(FString command)
{
	auto const pVar = IConsoleManager::Get().FindConsoleVariable(*command);
	if (pVar != nullptr)
	{
		float* v = m_ReservedFloatCmds.Find(command);
		if (v)
		{
			pVar->Set(*v);
		}
		else
		{
			UE_LOG(LogXeSSCapture, Warning, TEXT("Restore command not set %s."), *command);
		}
	}
	else
	{
		UE_LOG(LogXeSSCapture, Warning, TEXT("Restore command not found %s."), *command);
	}
}

void FXeSSCaptureModule::OnPostEngineInit()
{
	PXeSSCaptureViewExtension = FSceneViewExtensions::NewExtension<FXeSSCaptureViewExtension>();
}

void FXeSSCaptureModule::OnBeginFrame()
{
	if (!PXeSSCaptureViewExtension->IsActive())
	{
#if WITH_EDITOR
		if (GIsEditor && !GEditor->PlayWorld && !GIsPlayInEditorWorld)
		{
			CaptureFrameCount = 0;
		}
#endif
		if (CaptureFrameCount > 0)
		{
			if (CaptureDelayCount < CVarXeSSCaptureDelayTime.GetValueOnGameThread())
			{
				CaptureDelayCount += FApp::GetDeltaTime();
			}
			else
			{
				// reset
				CaptureDelayCount = 0.0f;

				// ensure all capture finished
				FlushRenderingCommands();

				TStringBuilder<32> DateTimeStringBuilder;
				DateTimeStringBuilder << FDateTime::Now().ToString();
				CVarXeSSCaptureDateTime->Set(DateTimeStringBuilder.GetData());
				PXeSSCaptureViewExtension->SetActive(true);
				XeSSCaptureFrameIndex = 0;
				CVarXeSSCaptureFrameIndex->Set(XeSSCaptureFrameIndex);

				// save previous frame settings
				m_bReservedSmoothFrameRate = GEngine->bSmoothFrameRate;
				m_bReservedUseFixedFrameRate = GEngine->bUseFixedFrameRate;
				m_fReservedFixedFrameRate = GEngine->FixedFrameRate;

				GEngine->bSmoothFrameRate = 0;
				GEngine->bUseFixedFrameRate = 1;
				GEngine->FixedFrameRate = CVarXeSSCaptureFixedFrameRate.GetValueOnGameThread();

				// override cmds
				m_ReservedCmds.Empty();
				m_ReservedFloatCmds.Empty();
				OverrideCommands(TEXT("r.SSR.ExperimentalDenoiser"), 1);
				OverrideCommands(TEXT("r.SSR.Cone"), 0);
				OverrideCommands(TEXT("r.SSR.Quality"), 4);
				OverrideCommands(TEXT("r.BloomQuality"), 0);
				OverrideCommands(TEXT("r.MotionBlurQuality"), 0);
				OverrideCommands(TEXT("r.LensFlareQuality"), 0);
				OverrideCommands(TEXT("r.LightShaftDownSampleFactor"), 1);
				OverrideCommands(TEXT("r.SeparateTranslucencyAutoDownsample"), 0);
				OverrideCommands(TEXT("r.MaxQualityMode"), 1);
				OverrideCommands(TEXT("r.SSS.HalfRes"), 0);
				OverrideCommands(TEXT("r.AmbientOcclusionMipLevelFactor"), 0);
				OverrideCommands(TEXT("r.DFFullResolution"), 1);
				OverrideCommands(TEXT("r.SecondaryScreenPercentage.GameViewport"), 100);
				OverrideCommands(TEXT("r.Shadow.MaxCSMResolution"), 4096);
				OverrideCommands(TEXT("r.TemporalAA.Algorithm"), 0);
				OverrideCommands(TEXT("r.ViewTextureMipBias.Offset"), -1);
				OverrideCommands(TEXT("ShowFlag.Particles"), 0);
				OverrideCommands(TEXT("r.LightShaftQuality"), 0);
				OverrideCommands(TEXT("r.Fog"), 0);
				OverrideCommands(TEXT("r.VolumetricFog"), 0);
				OverrideCommands(TEXT("r.DepthOfFieldQuality"), 0);
				OverrideCommands(TEXT("ShowFlag.Fog"), 0);
				OverrideCommands(TEXT("ShowFlag.VolumetricFog"), 0);
				OverrideCommands(TEXT("ShowFlag.AtmosphericFog"), 0);
				OverrideCommands(TEXT("r.MaxAnisotropy"), 16);
				OverrideCommands(TEXT("r.TemporalAACurrentFrameWeight"), 1);
				OverrideCommands(TEXT("r.NGX.DLSS.Enable"), 0);
				OverrideCommands(TEXT("r.ViewTextureMipBias.Min"), -1);
				OverrideCommands(TEXT("r.TemporalAA.AllowDownsampling"), 0);
				OverrideCommands(TEXT("r.TemporalAAUpsampleFiltered"), 1);
				OverrideCommands(TEXT("r.TemporalAACatmullRom"), 1);
				OverrideCommands(TEXT("r.TemporalAAFilterSize"), 1);
				OverrideCommands(TEXT("r.TemporalAA.Upsampling"), 1);
				OverrideCommands(TEXT("r.TemporalAASamples"), 8);
				OverrideCommandsFloat(TEXT("r.ScreenPercentage"), CVarXeSSCaptureScreenPercentage.GetValueOnGameThread());

				m_pReservedTemporalUpscaler = GTemporalUpscaler;
			}
		}
	}
	if (PXeSSCaptureViewExtension->IsActive())
	{
		PXeSSCaptureViewExtension->SetCaptureSR(false);
	}
}

void FXeSSCaptureModule::OnEndFrame()
{
	if (PXeSSCaptureViewExtension->IsActive())
	{
		PXeSSCaptureViewExtension->SetCaptureSR(true);
		XeSSCaptureSR::Capture();
		PXeSSCaptureViewExtension->SetCaptureSR(false);

		// capture count
		XeSSCaptureFrameIndex++;
		CVarXeSSCaptureFrameIndex->Set(XeSSCaptureFrameIndex);
		CaptureFrameCount--;
		if (CaptureFrameCount == 0)
		{
			// ensure all capture finished
			FlushRenderingCommands();

			// restore frame settings
			GEngine->bSmoothFrameRate = m_bReservedSmoothFrameRate;
			GEngine->bUseFixedFrameRate = m_bReservedUseFixedFrameRate;
			GEngine->FixedFrameRate = m_fReservedFixedFrameRate;
			GTemporalUpscaler = m_pReservedTemporalUpscaler;

			// restore cmds
			RestoreCommands(TEXT("r.SSR.ExperimentalDenoiser"));
			RestoreCommands(TEXT("r.SSR.Cone"));
			RestoreCommands(TEXT("r.SSR.Quality"));
			RestoreCommands(TEXT("r.BloomQuality"));
			RestoreCommands(TEXT("r.MotionBlurQuality"));
			RestoreCommands(TEXT("r.LensFlareQuality"));
			RestoreCommands(TEXT("r.LightShaftDownSampleFactor"));
			RestoreCommands(TEXT("r.SeparateTranslucencyAutoDownsample"));
			RestoreCommands(TEXT("r.MaxQualityMode"));
			RestoreCommands(TEXT("r.SSS.HalfRes"));
			RestoreCommands(TEXT("r.AmbientOcclusionMipLevelFactor"));
			RestoreCommands(TEXT("r.DFFullResolution"));
			RestoreCommands(TEXT("r.SecondaryScreenPercentage.GameViewport"));
			RestoreCommands(TEXT("r.Shadow.MaxCSMResolution"));
			RestoreCommands(TEXT("r.TemporalAA.Algorithm"));
			RestoreCommands(TEXT("r.ViewTextureMipBias.Offset"));
			RestoreCommands(TEXT("ShowFlag.Particles"));
			RestoreCommands(TEXT("r.LightShaftQuality"));
			RestoreCommands(TEXT("r.Fog"));
			RestoreCommands(TEXT("r.VolumetricFog"));
			RestoreCommands(TEXT("r.DepthOfFieldQuality"));
			RestoreCommands(TEXT("ShowFlag.Fog"));
			RestoreCommands(TEXT("ShowFlag.VolumetricFog"));
			RestoreCommands(TEXT("ShowFlag.AtmosphericFog"));
			RestoreCommands(TEXT("r.MaxAnisotropy"));
			RestoreCommands(TEXT("r.TemporalAACurrentFrameWeight"));
			RestoreCommands(TEXT("r.NGX.DLSS.Enable"));
			RestoreCommands(TEXT("r.ViewTextureMipBias.Min"));
			RestoreCommands(TEXT("r.TemporalAA.AllowDownsampling"));
			RestoreCommands(TEXT("r.TemporalAAUpsampleFiltered"));
			RestoreCommands(TEXT("r.TemporalAACatmullRom"));
			RestoreCommands(TEXT("r.TemporalAAFilterSize"));
			RestoreCommands(TEXT("r.TemporalAA.Upsampling"));
			RestoreCommands(TEXT("r.TemporalAASamples"));
			RestoreCommandsFloat(TEXT("r.ScreenPercentage"));

			PXeSSCaptureViewExtension->SetActive(false);
		}
	}
}

void FXeSSCaptureModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("XeSSCapture"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/XeSSCapture"), PluginShaderDir);

	// post engine init
	FCoreDelegates::OnPostEngineInit.AddRaw(this, &FXeSSCaptureModule::OnPostEngineInit);
	// begin frame
	FCoreDelegates::OnBeginFrame.AddRaw(this, &FXeSSCaptureModule::OnBeginFrame);
	// end frame
	FCoreDelegates::OnEndFrame.AddRaw(this, &FXeSSCaptureModule::OnEndFrame);
}

void FXeSSCaptureModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	FCoreDelegates::OnBeginFrame.RemoveAll(this);
	FCoreDelegates::OnEndFrame.RemoveAll(this);
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);

	PXeSSCaptureViewExtension = nullptr;
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FXeSSCaptureModule, XeSSCapture)