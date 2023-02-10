#include "XeSSCaptureSR.h"
#include "XeSSCapture.h"

#include "Engine/Engine.h"
#include "CanvasTypes.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/SceneViewPort.h"
#include "SceneViewExtension.h"
#include "EngineModule.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/LocalPlayer.h"
#include "LegacyScreenPercentageDriver.h"
// sample 256
#include "Jitter.h"

static TAutoConsoleVariable<int32> CVarXeSSCaptureSampleIndex(
	TEXT("r.XeSSCapture.SampleIndex"),
	0,
	TEXT("The sample index of this XeSS SR capture."),
	ECVF_RenderThreadSafe);

static void CaptureSample(UTextureRenderTarget2D* CaptureRenderTexture, FIntPoint& TargetSize, UWorld* World, ULocalPlayer* LP, FSceneViewProjectionData& ProjectionData)
{
	FTextureRenderTargetResource* RenderTargetResource = CaptureRenderTexture->GameThread_GetRenderTargetResource();

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		RenderTargetResource,
		World->Scene,
		FEngineShowFlags(ESFIM_Game))
		.SetRealtimeUpdate(true));

	ViewFamily.EngineShowFlags.ScreenPercentage = true;
	ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(ViewFamily, FLegacyScreenPercentageDriver::GetCVarResolutionFraction(), true));

	ViewFamily.ViewExtensions = GEngine->ViewExtensions->GatherActiveExtensions(FSceneViewExtensionContext(World->Scene));
	for (auto ViewExt : ViewFamily.ViewExtensions)
	{
		ViewExt->SetupViewFamily(ViewFamily);
	}

	FSceneViewInitOptions ViewInitOptions;

#if ENGINE_MAJOR_VERSION < 5
	if (!LP->CalcSceneViewInitOptions(ViewInitOptions, LP->ViewportClient->Viewport, nullptr, eSSP_FULL))
#else
	if (!LP->CalcSceneViewInitOptions(ViewInitOptions, LP->ViewportClient->Viewport))
#endif
	{
		return;
	}
	ViewInitOptions.SetViewRectangle(FIntRect(0, 0, TargetSize.X, TargetSize.Y));
	ViewInitOptions.ViewFamily = &ViewFamily;
	ViewInitOptions.ViewOrigin = ProjectionData.ViewOrigin;
	ViewInitOptions.ViewRotationMatrix = ProjectionData.ViewRotationMatrix;
	ViewInitOptions.ProjectionMatrix = ProjectionData.ProjectionMatrix;

	FSceneView* NewView = new FSceneView(ViewInitOptions);

	ViewFamily.Views.Add(NewView);
	for (int ViewExt = 0; ViewExt < ViewFamily.ViewExtensions.Num(); ViewExt++)
	{
		ViewFamily.ViewExtensions[ViewExt]->SetupView(ViewFamily, *NewView);
	}

	FCanvas Canvas(RenderTargetResource, NULL, World, World->Scene->GetFeatureLevel());
	Canvas.Clear(FLinearColor::Transparent);
	GetRendererModule().BeginRenderingViewFamily(&Canvas, &ViewFamily);
	FlushRenderingCommands();
}

void XeSSCaptureSR::Capture()
{
	FWorldContext* world = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
	UWorld* World = world->World();
	if (!World)
		return;

	UGameViewportClient* GameViewport = GEngine->GameViewport;
	if (!GameViewport)
		return;

	FSceneViewport* SceneViewPort = GameViewport->GetGameViewport();
	if (!SceneViewPort)
		return;

	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(World, 0);
	if (!PlayerController)
		return;

	ULocalPlayer* LP = PlayerController->GetLocalPlayer();
	if (!LP || !LP->ViewportClient)
		return;

	// get the projection data
	FSceneViewProjectionData ProjectionData;
#if ENGINE_MAJOR_VERSION < 5
	if (!LP->GetProjectionData(LP->ViewportClient->Viewport, eSSP_FULL, /*out*/ ProjectionData))
#else
	if (!LP->GetProjectionData(LP->ViewportClient->Viewport, ProjectionData))
#endif
		return;

//#if WITH_EDITOR
	FIntPoint TargetSize = ProjectionData.GetConstrainedViewRect().Size();
//#else
//	FIntPoint TargetSize = FIntPoint(GSystemResolution.ResX, GSystemResolution.ResY);
//#endif

	UTextureRenderTarget2D* CaptureRenderTexture = NewObject<UTextureRenderTarget2D>();
	CaptureRenderTexture->AddToRoot();
	CaptureRenderTexture->ClearColor = FLinearColor::Transparent;
	CaptureRenderTexture->TargetGamma = 1.0;
	CaptureRenderTexture->InitCustomFormat(TargetSize.X, TargetSize.Y, PF_B8G8R8A8, false);

	// capture for 256 samples
	static const auto TemporalAASamples = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAASamples"));
	TemporalAASamples->Set(64);

	for (int i = 0; i < 256; i++)
	{
		CVarXeSSCaptureSampleIndex->Set(i);
		CaptureSample(CaptureRenderTexture, TargetSize, World, LP, ProjectionData);
	}

	// restore to 32 samples
	TemporalAASamples->Set(8);

	CaptureRenderTexture->RemoveFromRoot();
	CaptureRenderTexture = nullptr;
}