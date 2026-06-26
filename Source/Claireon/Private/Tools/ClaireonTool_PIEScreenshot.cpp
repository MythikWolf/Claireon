// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEScreenshot.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "ImageUtils.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "EngineUtils.h"

FString ClaireonTool_PIEScreenshot::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEScreenshot::GetOperation() const { return TEXT("screenshot"); }

FString ClaireonTool_PIEScreenshot::GetDescription() const
{
    return TEXT("Capture a PIE screenshot and return its on-disk path. Default: the active player viewport (behind-player, includes HUD). Positional: pass 'view' (front/back/left/right/front_left/front_right/back_left/back_right/top) to orbit a target (default: the player pawn) -- or explicit 'camera_location'+'camera_rotation' -- to render the live scene from any angle (scene only, no HUD). Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEScreenshot::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output filename without extension. Default: screenshot_<timestamp>"));
		Properties->SetObjectField(TEXT("filename"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output directory. Default: Saved/Screenshots/"));
		Properties->SetObjectField(TEXT("directory"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"), TEXT("Resolution multiplier for HighResShot (e.g., 2.0 for 2x viewport). Default: 1.0"));
		Prop->SetNumberField(TEXT("default"), 1.0);
		Properties->SetObjectField(TEXT("resolutionMultiplier"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Prop->SetStringField(TEXT("description"), TEXT("Include UMG/Slate UI overlays in the screenshot. Default: true"));
		Properties->SetObjectField(TEXT("show_ui"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Orbit-preset camera around a target: front|back|left|right|front_left|front_right|back_left|back_right|top. Renders the live scene from that angle (no HUD)."));
		Properties->SetObjectField(TEXT("view"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Actor to orbit for 'view' (name/label substring). Default: the player pawn."));
		Properties->SetObjectField(TEXT("target"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"), TEXT("Orbit distance from the target for 'view'. Default: 350."));
		Properties->SetObjectField(TEXT("distance"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"), TEXT("Vertical offset of the look-at point above the target's center for 'view'. Default: 0."));
		Properties->SetObjectField(TEXT("height"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"), TEXT("Field of view (degrees) for positional captures. Default: 50."));
		Properties->SetObjectField(TEXT("fov"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("array"));
		Prop->SetStringField(TEXT("description"), TEXT("Explicit camera world location [x,y,z]. Overrides 'view'."));
		Properties->SetObjectField(TEXT("camera_location"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("array"));
		Prop->SetStringField(TEXT("description"), TEXT("Explicit camera rotation [pitch,yaw,roll] for 'camera_location'. Default: zero (or aim manually)."));
		Properties->SetObjectField(TEXT("camera_rotation"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEScreenshot::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	// Resolve output filename
	FString Filename;
	if (!Arguments->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		FDateTime Now = FDateTime::Now();
		Filename = FString::Printf(TEXT("screenshot_%04d%02d%02d_%02d%02d%02d"),
			Now.GetYear(), Now.GetMonth(), Now.GetDay(),
			Now.GetHour(), Now.GetMinute(), Now.GetSecond());
	}

	// Resolve output directory
	FString Directory;
	if (!Arguments->TryGetStringField(TEXT("directory"), Directory) || Directory.IsEmpty())
	{
		Directory = FPaths::ProjectSavedDir() / TEXT("Screenshots");
	}

	// Resolution multiplier
	double ResolutionMultiplier = 1.0;
	Arguments->TryGetNumberField(TEXT("resolutionMultiplier"), ResolutionMultiplier);

	// Ensure directory exists
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);

	// Build full output path (without extension; Unreal appends .png)
	FString FullPath = Directory / Filename;
	const FString FinalPath = FullPath + TEXT(".png");

	// --- POSITIONAL MODE: spawn a transient SceneCapture2D in the PIE world and render from an arbitrary
	//     vantage to a render target -> PNG (focus-independent). Triggered by an explicit camera_location,
	//     or a "view" orbit preset around a target (default: the player pawn). With neither, falls through
	//     to the default behind-the-player game-viewport grab below. ---
	{
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		FString ViewPreset;
		const bool bExplicit = Arguments->TryGetArrayField(TEXT("camera_location"), LocArr) && LocArr && LocArr->Num() >= 3;
		const bool bOrbit = Arguments->TryGetStringField(TEXT("view"), ViewPreset) && !ViewPreset.IsEmpty();
		if (bExplicit || bOrbit)
		{
			UWorld* World = GEditor->PlayWorld;
			if (!World)
			{
				return MakeErrorResult(TEXT("No active PIE world (start PIE before a positional capture)"));
			}

			double Fov = 50.0; Arguments->TryGetNumberField(TEXT("fov"), Fov);
			double Distance = 350.0; Arguments->TryGetNumberField(TEXT("distance"), Distance);
			double Height = 0.0; Arguments->TryGetNumberField(TEXT("height"), Height);

			FVector CamLoc = FVector::ZeroVector;
			FRotator CamRot = FRotator::ZeroRotator;

			if (bExplicit)
			{
				CamLoc = FVector((*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber());
				const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
				if (Arguments->TryGetArrayField(TEXT("camera_rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
				{
					CamRot = FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
				}
			}
			else
			{
				AActor* Target = nullptr;
				FString TargetName;
				if (Arguments->TryGetStringField(TEXT("target"), TargetName) && !TargetName.IsEmpty())
				{
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						if (It->GetActorNameOrLabel().Contains(TargetName) || It->GetName().Contains(TargetName))
						{
							Target = *It;
							break;
						}
					}
				}
				else
				{
					Target = UGameplayStatics::GetPlayerPawn(World, 0);
				}
				if (!Target)
				{
					return MakeErrorResult(TEXT("Orbit target not found (no player pawn / no actor matching 'target')"));
				}

				// Use the actor location (capsule/pivot), NOT GetActorBounds -- a pawn's bounds include its
				// camera boom + follow camera, which skews the look-at point far up and behind.
				const FVector Center = Target->GetActorLocation() + FVector(0.0, 0.0, Height);
				const FVector Fwd = Target->GetActorForwardVector();
				const FVector Right = Target->GetActorRightVector();

				const FString V = ViewPreset.ToLower();
				FVector Dir = FVector::ZeroVector;
				if (V == TEXT("front")) Dir = Fwd;
				else if (V == TEXT("back")) Dir = -Fwd;
				else if (V == TEXT("left")) Dir = -Right;
				else if (V == TEXT("right")) Dir = Right;
				else if (V == TEXT("front_left")) Dir = Fwd - Right;
				else if (V == TEXT("front_right")) Dir = Fwd + Right;
				else if (V == TEXT("back_left")) Dir = -Fwd - Right;
				else if (V == TEXT("back_right")) Dir = -Fwd + Right;
				else if (V == TEXT("top")) Dir = FVector(0.0, 0.0, 1.0);
				else
				{
					return MakeErrorResult(FString::Printf(TEXT("Unknown view '%s' (front|back|left|right|front_left|front_right|back_left|back_right|top)"), *ViewPreset));
				}
				CamLoc = Center + Dir.GetSafeNormal() * Distance;
				CamRot = (Center - CamLoc).Rotation();
			}

			FViewport* VP = (GEngine && GEngine->GameViewport) ? GEngine->GameViewport->Viewport : nullptr;
			APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
			if (!VP || !PC)
			{
				return MakeErrorResult(TEXT("No game viewport / player controller for positional capture"));
			}

			// Render the vantage through the GAME viewport so it inherits the game's already-converged
			// lighting + exposure -- a one-shot SceneCapture comes out black on near subjects (the bright
			// scene/character crushes its un-adapted auto-exposure). Spawn a temp camera, make it the
			// player's view target for a single Draw, read it back, then restore the player's view.
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.ObjectFlags |= RF_Transient;
			ACameraActor* CamActor = World->SpawnActor<ACameraActor>(CamLoc, CamRot, SpawnParams);
			if (!CamActor)
			{
				return MakeErrorResult(TEXT("Failed to spawn camera actor in the PIE world"));
			}
			if (UCameraComponent* CamComp = CamActor->GetCameraComponent())
			{
				CamComp->SetFieldOfView(static_cast<float>(Fov));
			}

			AActor* PrevViewTarget = PC->GetViewTarget();
			PC->SetViewTarget(CamActor);
			if (PC->PlayerCameraManager) { PC->PlayerCameraManager->UpdateCamera(0.0f); }
			VP->Draw(false);

			TArray<FColor> Bitmap;
			const FIntPoint Size = VP->GetSizeXY();
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
			ReadFlags.SetLinearToGamma(false);
			const bool bRead = VP->ReadPixels(Bitmap, ReadFlags, FIntRect(0, 0, Size.X, Size.Y)) && Bitmap.Num() > 0;

			// Restore the player's view and re-render so the live window isn't left on the temp camera.
			PC->SetViewTarget(PrevViewTarget);
			if (PC->PlayerCameraManager) { PC->PlayerCameraManager->UpdateCamera(0.0f); }
			VP->Draw(false);
			CamActor->Destroy();

			if (!bRead)
			{
				return MakeErrorResult(TEXT("Viewport ReadPixels returned no data (positional)"));
			}
			for (FColor& Px : Bitmap) { Px.A = 255; }

			TArray<uint8> CompressedPNG;
			FImageUtils::ThumbnailCompressImageArray(Size.X, Size.Y, Bitmap, CompressedPNG);
			if (CompressedPNG.Num() == 0 || !FFileHelper::SaveArrayToFile(CompressedPNG, *FinalPath))
			{
				return MakeErrorResult(FString::Printf(TEXT("Failed to encode/write PNG to %s"), *FinalPath));
			}

			TSharedPtr<FJsonObject> PData = MakeShared<FJsonObject>();
			PData->SetStringField(TEXT("file_path"), FinalPath);
			PData->SetStringField(TEXT("resolution"), FString::Printf(TEXT("%dx%d"), Size.X, Size.Y));
			PData->SetNumberField(TEXT("size_bytes"), static_cast<double>(CompressedPNG.Num()));
			PData->SetStringField(TEXT("camera_location"), CamLoc.ToCompactString());
			PData->SetStringField(TEXT("camera_rotation"), CamRot.ToCompactString());
			return MakeSuccessResult(PData, FString::Printf(TEXT("Positional screenshot saved to %s"), *FinalPath));
		}
	}

	// Default capture SYNCHRONOUSLY: force a fresh viewport Draw, read the pixels back ourselves, and write
	// the PNG. The old FScreenshotRequest path only captured on the next ENGINE-driven frame, which never
	// arrives while the editor window is backgrounded (UE throttles rendering) -- it silently produced a
	// 0-byte / missing file. An explicit Draw() renders regardless of focus.
	int64 SizeBytes = 0;
	FString ResolutionStr = TEXT("unknown");

	FViewport* Viewport = (GEngine && GEngine->GameViewport) ? GEngine->GameViewport->Viewport : nullptr;
	if (!Viewport)
	{
		return MakeErrorResult(TEXT("No active PIE game viewport (start PIE before capturing)"));
	}
	const FIntPoint Size = Viewport->GetSizeXY();

	if (ResolutionMultiplier > 1.0)
	{
		// >1x: fall back to HighResShot (still engine-frame driven -- reliable only when the window
		// can render). Routed to the requested path via the filename= arg.
		const FString HighResCommand = FString::Printf(TEXT("HighResShot %.2f filename=\"%s\""), ResolutionMultiplier, *FinalPath);
		GEngine->Exec(nullptr, *HighResCommand);
		ResolutionStr = FString::Printf(TEXT("%dx%d"), static_cast<int32>(Size.X * ResolutionMultiplier), static_cast<int32>(Size.Y * ResolutionMultiplier));
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*FinalPath))
		{
			SizeBytes = FPlatformFileManager::Get().GetPlatformFile().FileSize(*FinalPath);
		}
	}
	else
	{
		Viewport->Draw(false);

		TArray<FColor> Bitmap;
		FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
		ReadFlags.SetLinearToGamma(false);
		if (!Viewport->ReadPixels(Bitmap, ReadFlags, FIntRect(0, 0, Size.X, Size.Y)) || Bitmap.Num() == 0)
		{
			return MakeErrorResult(TEXT("Viewport ReadPixels returned no data"));
		}
		// Back-buffer alpha is unreliable; force opaque so the PNG isn't transparent.
		for (FColor& Px : Bitmap)
		{
			Px.A = 255;
		}

		TArray<uint8> CompressedPNG;
		FImageUtils::ThumbnailCompressImageArray(Size.X, Size.Y, Bitmap, CompressedPNG);
		if (CompressedPNG.Num() == 0 || !FFileHelper::SaveArrayToFile(CompressedPNG, *FinalPath))
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to encode/write PNG to %s"), *FinalPath));
		}
		SizeBytes = CompressedPNG.Num();
		ResolutionStr = FString::Printf(TEXT("%dx%d"), Size.X, Size.Y);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("file_path"), FinalPath);
	Data->SetStringField(TEXT("resolution"), ResolutionStr);
	Data->SetNumberField(TEXT("size_bytes"), static_cast<double>(SizeBytes));

	FString Summary = FString::Printf(TEXT("Screenshot saved to %s"), *FinalPath);
	return MakeSuccessResult(Data, Summary);
}
