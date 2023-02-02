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
#include "Misc/FileHelper.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
// sample 256
#include "Jitter.h"

static TAutoConsoleVariable<int32> CVarTemporalAASampleOverride(
	TEXT("r.XeSSCapture.TemporalAASampleOverride"),
	0,
	TEXT("If we need to override TAA samples."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarTemporalAASampleOverrideX(
	TEXT("r.XeSSCapture.TemporalAASampleOverrideX"),
	0.0f,
	TEXT("Override TAA sample X."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarTemporalAASampleOverrideY(
	TEXT("r.XeSSCapture.TemporalAASampleOverrideY"),
	0.0f,
	TEXT("Override TAA sample Y."),
	ECVF_RenderThreadSafe);

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

	if (!LP->CalcSceneViewInitOptions(ViewInitOptions, LP->ViewportClient->Viewport, nullptr, eSSP_FULL))
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
	if (!LP->GetProjectionData(LP->ViewportClient->Viewport, eSSP_FULL, /*out*/ ProjectionData))
		return;

#if WITH_EDITOR
	FIntPoint TargetSize = ProjectionData.GetConstrainedViewRect().Size();
#else
	FIntPoint TargetSize = FIntPoint(GSystemResolution.ResX, GSystemResolution.ResY);
#endif

	UTextureRenderTarget2D* CaptureRenderTexture = NewObject<UTextureRenderTarget2D>();
	CaptureRenderTexture->AddToRoot();
	CaptureRenderTexture->ClearColor = FLinearColor::Transparent;
	CaptureRenderTexture->TargetGamma = 1.0;
	CaptureRenderTexture->InitCustomFormat(TargetSize.X, TargetSize.Y, PF_B8G8R8A8, false);

	CVarTemporalAASampleOverride->Set(1);

	for (int i = 0; i < 256; i++)
	{
		static const auto OutputDirectory = IConsoleManager::Get().FindConsoleVariable(TEXT("r.XeSSCapture.OutputDirectory"));
		static const auto DateTime = IConsoleManager::Get().FindConsoleVariable(TEXT("r.XeSSCapture.DateTime"));
		static const auto FrameIndex = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.XeSSCapture.FrameIndex"));

		if (FrameIndex->GetValueOnGameThread() == 0)
		{
			SaveSampleToJson(FString::Printf(TEXT("%s\\%s\\sr\\samples\\srsample_%d.json"), *OutputDirectory->GetString(), *DateTime->GetString(), i), samples_256[i][0], samples_256[i][1]);
		}

		CVarXeSSCaptureSampleIndex->Set(i);
		CVarTemporalAASampleOverrideX->Set(samples_256[i][0]);
		CVarTemporalAASampleOverrideY->Set(samples_256[i][1]);

		CaptureSample(CaptureRenderTexture, TargetSize, World, LP, ProjectionData);
	}

	CVarTemporalAASampleOverride->Set(0);

	CaptureRenderTexture->RemoveFromRoot();
	CaptureRenderTexture = nullptr;
}

void XeSSCaptureSR::SaveSampleToJson(const FString& fileName, float sampleX, float sampleY)
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