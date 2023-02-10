// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogXeSSCapture, Log, All);

class ITemporalUpscaler;
class FXeSSCaptureViewExtension;
class FXeSSCaptureModule : public IModuleInterface
{
public:
	void OnPostEngineInit();
	void OnBeginFrame();
	void OnEndFrame();

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TSharedPtr<FXeSSCaptureViewExtension, ESPMode::ThreadSafe> PXeSSCaptureViewExtension;

	uint32 m_bReservedSmoothFrameRate;
	uint32 m_bReservedUseFixedFrameRate;
	float m_fReservedFixedFrameRate;
	const ITemporalUpscaler* m_pReservedTemporalUpscaler;

	void OverrideCommands(FString command, int value);
	void RestoreCommands(FString command);
	TMap<FString, int> m_ReservedCmds;

	void OverrideCommandsFloat(FString command, float value);
	void RestoreCommandsFloat(FString command);
	TMap<FString, float> m_ReservedFloatCmds;
};
