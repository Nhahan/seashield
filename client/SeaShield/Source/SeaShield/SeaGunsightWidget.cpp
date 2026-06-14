#include "SeaGunsightWidget.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Fonts/SlateFontInfo.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

#include "SeaBallistics.h"
#include "SeaGunnerPawn.h"
#include "SeaWorldFrame.h"

// Reticle + target-designator painter.
class SSeaGunsight final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSeaGunsight) {}
	SLATE_ARGUMENT(TWeakObjectPtr<const USeaGunsightWidget>, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;
		SetCanTick(false);
		ForceVolatile(true);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(8.0, 8.0); }

	static void Line(FSlateWindowElementList& Out, int32 Layer, const FPaintGeometry& Geo,
	                 const FVector2D& A, const FVector2D& B, const FLinearColor& C, float W = 1.2f)
	{
		FSlateDrawElement::MakeLines(Out, Layer, Geo, {A, B}, ESlateDrawEffect::None, C, true, W);
	}

	// High-contrast line: a dark halo under the coloured stroke so HUD symbology
	// survives both bright sky and bright sea (the #1 readability complaint).
	static void OutLine(FSlateWindowElementList& Out, int32 Layer, const FPaintGeometry& Geo,
	                    const FVector2D& A, const FVector2D& B, const FLinearColor& C, float W = 2.0f)
	{
		FSlateDrawElement::MakeLines(Out, Layer, Geo, {A, B}, ESlateDrawEffect::None,
		                             FLinearColor(0, 0, 0, 0.55f), true, W + 2.4f);
		FSlateDrawElement::MakeLines(Out, Layer, Geo, {A, B}, ESlateDrawEffect::None, C, true, W);
	}

	// Centred high-contrast text (dark plate behind), returns nothing.
	static void Label(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
	                  const FVector2D& TopLeft, const FVector2D& Box, const FString& Str,
	                  const FSlateFontInfo& Font, const FLinearColor& C)
	{
		static const FSlateColorBrush Plate(FLinearColor::White);
		FSlateDrawElement::MakeBox(
		    Out, Layer,
		    Geometry.ToPaintGeometry(FVector2f(Box.X, Box.Y),
		                             FSlateLayoutTransform(FVector2f(TopLeft.X - 6, TopLeft.Y - 2))),
		    &Plate, ESlateDrawEffect::None, FLinearColor(0.0f, 0.02f, 0.01f, 0.55f));
		FSlateDrawElement::MakeText(
		    Out, Layer + 1,
		    Geometry.ToPaintGeometry(FVector2f(Box.X, Box.Y),
		                             FSlateLayoutTransform(FVector2f(TopLeft.X, TopLeft.Y))),
		    Str, Font, ESlateDrawEffect::None, C);
	}

	static void Circle(FSlateWindowElementList& Out, int32 Layer, const FPaintGeometry& Geo,
	                   const FVector2D& C, float R, const FLinearColor& Col, float W = 1.5f)
	{
		TArray<FVector2D> Pts;
		constexpr int32 N = 24;
		for (int32 i = 0; i <= N; ++i)
		{
			const float A = (2.0f * PI * i) / N;
			Pts.Add(C + FVector2D(FMath::Cos(A), FMath::Sin(A)) * R);
		}
		FSlateDrawElement::MakeLines(Out, Layer, Geo, Pts, ESlateDrawEffect::None, Col, true, W);
	}

	static void Diamond(FSlateWindowElementList& Out, int32 Layer, const FPaintGeometry& Geo,
	                    const FVector2D& C, float R, const FLinearColor& Col, float W = 1.5f)
	{
		const TArray<FVector2D> Pts = {C + FVector2D(0, -R), C + FVector2D(R, 0), C + FVector2D(0, R),
		                               C + FVector2D(-R, 0), C + FVector2D(0, -R)};
		FSlateDrawElement::MakeLines(Out, Layer, Geo, Pts, ESlateDrawEffect::None, Col, true, W);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry,
	                      const FSlateRect& Cull, FSlateWindowElementList& Out, int32 Layer,
	                      const FWidgetStyle& Style, bool bEnabled) const override
	{
		const USeaGunsightWidget* Data = Owner.Get();
		if (Data == nullptr)
		{
			return Layer;
		}
		const FVector2D Size = Geometry.GetLocalSize();
		const FVector2D Center = Size * 0.5f;
		const FPaintGeometry Geo = Geometry.ToPaintGeometry();

		const float Pulse = Data->Pulse();

		// Center reticle: a gap-cross, outlined so it survives bright cloud/sea.
		const FLinearColor R = Data->ReticleColor;
		OutLine(Out, Layer, Geo, Center + FVector2D(-22, 0), Center + FVector2D(-7, 0), R, 1.6f);
		OutLine(Out, Layer, Geo, Center + FVector2D(7, 0), Center + FVector2D(22, 0), R, 1.6f);
		OutLine(Out, Layer, Geo, Center + FVector2D(0, -22), Center + FVector2D(0, -7), R, 1.6f);
		OutLine(Out, Layer, Geo, Center + FVector2D(0, 7), Center + FVector2D(0, 22), R, 1.6f);

		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 11);
		const FSlateFontInfo RangeFont = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 13);
		for (const USeaGunsightWidget::FMarker& M : Data->Markers())
		{
			if (M.Kind != static_cast<uint8>(ESeaEntityKind::Target))
			{
				continue;  // Designate the threat; rockets are visible as meshes.
			}
			const FVector2D P = M.Local;
			// The ASM is a few pixels at battle range — make the box do the seeing:
			// it grows as the threat closes and goes red inside ~2.5 km, pulsing.
			const float Prox = FMath::Clamp((8000.0f - M.RangeM) / 7000.0f, 0.0f, 1.0f);
			const float H = FMath::Lerp(16.0f, 40.0f, Prox);
			const bool bClose = M.RangeM < 2500.0f;
			FLinearColor C = bClose ? FLinearColor(1.0f, 0.12f, 0.10f, 1.0f) : Data->TargetColor;
			C.A = 0.72f + 0.28f * Pulse;
			const float k = H * 0.42f;
			for (int sx = -1; sx <= 1; sx += 2)
			{
				for (int sy = -1; sy <= 1; sy += 2)
				{
					const FVector2D Corner = P + FVector2D(sx * H, sy * H);
					OutLine(Out, Layer + 1, Geo, Corner, Corner - FVector2D(sx * k, 0), C, 2.2f);
					OutLine(Out, Layer + 1, Geo, Corner, Corner - FVector2D(0, sy * k), C, 2.2f);
				}
			}
			// A small marked centre so the eye finds the speck inside the box.
			OutLine(Out, Layer + 1, Geo, P + FVector2D(-5, 0), P + FVector2D(5, 0), C, 1.6f);
			OutLine(Out, Layer + 1, Geo, P + FVector2D(0, -5), P + FVector2D(0, 5), C, 1.6f);
			Label(Out, Layer + 2, Geometry, FVector2D(P.X + H + 6, P.Y - 8), FVector2D(110, 18),
			      FString::Printf(TEXT("%.1f KM"), M.RangeM / 1000.0f), RangeFont, C);
		}

		// Kill marks: a fading red "X KILL" at each recent kill location.
		for (const USeaGunsightWidget::FKillMark& K : Data->KillMarks())
		{
			if (!K.bOnScreen)
			{
				continue;
			}
			const float A = FMath::Clamp(1.0f - K.AgeS / 1.6f, 0.0f, 1.0f);
			const FLinearColor KC(0.4f, 1.0f, 0.5f, A);  // green = a confirmed splash
			const float r = 13.0f;
			OutLine(Out, Layer + 2, Geo, K.Local + FVector2D(-r, -r), K.Local + FVector2D(r, r), KC, 2.6f);
			OutLine(Out, Layer + 2, Geo, K.Local + FVector2D(-r, r), K.Local + FVector2D(r, -r), KC, 2.6f);
			Label(Out, Layer + 2, Geometry, FVector2D(K.Local.X + r + 4, K.Local.Y - 8),
			      FVector2D(70, 16), TEXT("KILL"), Font, KC);
		}

		// --- Fire-control aim aids ---------------------------------------------
		const USeaGunsightWidget::FAimPoint LeadPt = Data->LeadPoint();
		const USeaGunsightWidget::FAimPoint ImpactPt = Data->ImpactPoint();
		const bool bSolved = Data->HasSolution();

		// Lead ghost ("AIM HERE"): a cyan diamond the player chases — outlined.
		if (LeadPt.bValid && LeadPt.bOnScreen)
		{
			Diamond(Out, Layer + 3, Geo, LeadPt.Local, 14.0f, FLinearColor(0, 0, 0, 0.55f), 4.0f);
			Diamond(Out, Layer + 3, Geo, LeadPt.Local, 14.0f, Data->LeadColor, 2.0f);
			Label(Out, Layer + 4, Geometry, FVector2D(LeadPt.Local.X + 18, LeadPt.Local.Y - 7),
			      FVector2D(64, 16), TEXT("AIM"), Font, Data->LeadColor);
		}

		// Impact pipper: where the current bore's salvo will pass. Filled dot +
		// ring; green and emphasised on SOLUTION.
		if (ImpactPt.bValid && ImpactPt.bOnScreen)
		{
			const FLinearColor PipCol = bSolved ? Data->SolutionColor : Data->ImpactColor;
			if (LeadPt.bValid && LeadPt.bOnScreen && !bSolved)
			{
				Line(Out, Layer + 3, Geo, ImpactPt.Local, LeadPt.Local,
				     FLinearColor(PipCol.R, PipCol.G, PipCol.B, 0.30f), 1.0f);
			}
			// Dispersion footprint: the salvo's pattern at this range. It is
			// angular (mrad), so it balloons with distance — you can SEE why a far
			// shot is a gamble and a tighter/wider pattern is a real trade-off.
			if (ImpactPt.FootprintPx > 6.0f)
			{
				Circle(Out, Layer + 3, Geo, ImpactPt.Local,
				       FMath::Min(ImpactPt.FootprintPx, 260.0f),
				       FLinearColor(PipCol.R, PipCol.G, PipCol.B, 0.32f), 1.2f);
			}
			Circle(Out, Layer + 4, Geo, ImpactPt.Local, 11.0f, FLinearColor(0, 0, 0, 0.55f), 4.0f);
			Circle(Out, Layer + 4, Geo, ImpactPt.Local, 11.0f, PipCol, 2.0f);
			Circle(Out, Layer + 4, Geo, ImpactPt.Local, 2.5f, PipCol, 3.0f);  // filled-ish centre
			if (bSolved)
			{
				Circle(Out, Layer + 4, Geo, ImpactPt.Local, 11.0f + 5.0f * Pulse,
				       FLinearColor(Data->SolutionColor.R, Data->SolutionColor.G,
				                    Data->SolutionColor.B, 0.6f * (1.0f - Pulse)),
				       2.0f);  // expanding lock ring
			}
		}

		// Central FIRE / LEAD cue — the single most important state, promoted out
		// of the tiny console into the player's focus (just under the reticle).
		const USeaGunsightWidget::FThreatReadout ThreatState = Data->Threat();
		if (ThreatState.bValid)
		{
			// Engagement envelope: outside the effective band even a geometric
			// solution is a poor shot (long flight = the target out-maneuvers it;
			// too close = no time to correct). Gate FIRE on it.
			constexpr float kEnvMinM = 600.0f;
			constexpr float kEnvMaxM = 4500.0f;
			const FSlateFontInfo CueFont = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 18);
			FString Cue;
			FLinearColor CueCol;
			float CueW;
			// Range gates the cue FIRST (independent of marker lock), so the "wait
			// until it's in range" lesson actually fires at long range — that is
			// the whole "few good shots" point of an unguided engagement.
			const FLinearColor Amber(1.0f, 0.6f, 0.2f, 0.92f);
			if (ThreatState.RangeM > kEnvMaxM)
			{
				Cue = TEXT("HOLD - CLOSE THE RANGE");
				CueCol = Amber;
				CueW = 360.0f;
			}
			else if (ThreatState.RangeM < kEnvMinM)
			{
				Cue = TEXT("HOLD - TOO CLOSE");
				CueCol = Amber;
				CueW = 280.0f;
			}
			else if (bSolved)
			{
				Cue = TEXT(">> FIRE <<");
				CueCol = FLinearColor(0.35f, 1.0f, 0.45f, 0.7f + 0.3f * Pulse);
				CueW = 160.0f;
			}
			else
			{
				Cue = TEXT("LEAD - MATCH MARKERS");
				CueCol = FLinearColor(1.0f, 0.78f, 0.20f, 0.92f);
				CueW = 320.0f;
			}
			Label(Out, Layer + 5, Geometry, FVector2D(Center.X - CueW * 0.5f, Center.Y + 34.0f),
			      FVector2D(CueW, 26.0f), Cue, CueFont, CueCol);
		}

		// Lead-error correction after a salvo (brief): how far / which way the
		// threat slipped out of the committed solution. Teaches the unguided lead.
		const USeaNetSubsystem::FSeaLeadError Lead = Data->LeadError();
		if (Lead.bValid && Lead.AgeS < 3.5f)
		{
			// Correction in the gunner's frame: lateral (aim L/R) and vertical
			// (raise/lower) are the direct mouse moves; range (long/short) is
			// shown only when the aim axes are already tight.
			FString S = FString::Printf(TEXT("MISS %.0f m"), Lead.MissM);
			if (Lead.LateralM > 4.0f) { S += TEXT("   AIM RIGHT"); }
			else if (Lead.LateralM < -4.0f) { S += TEXT("   AIM LEFT"); }
			if (Lead.UpM > 4.0f) { S += TEXT("   RAISE"); }
			else if (Lead.UpM < -4.0f) { S += TEXT("   LOWER"); }
			if (FMath::Abs(Lead.LateralM) < 4.0f && FMath::Abs(Lead.UpM) < 4.0f)
			{
				if (Lead.LongM > 6.0f) { S += TEXT("   TARGET LONG"); }
				else if (Lead.LongM < -6.0f) { S += TEXT("   TARGET SHORT"); }
			}
			const float A = FMath::Clamp(3.5f - Lead.AgeS, 0.0f, 1.0f);
			Label(Out, Layer + 5, Geometry, FVector2D(Center.X - 200, Center.Y + 64),
			      FVector2D(400, 22), S, FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 14),
			      FLinearColor(1.0f, 0.55f, 0.2f, 0.92f * A));
		}

		// Threat read-out: range / closing / time-to-impact, top-center, on a dark
		// plate so it is readable against the sky. TTI drives an urgency colour.
		const USeaGunsightWidget::FThreatReadout Threat = Data->Threat();
		if (Threat.bValid)
		{
			FLinearColor Warn = Data->TargetColor;
			if (Threat.TtiS > 0.0f && Threat.TtiS < 6.0f)
			{
				Warn = FLinearColor(1.0f, 0.25f, 0.22f, 0.8f + 0.2f * Pulse);  // imminent
			}
			else if (Threat.TtiS > 0.0f && Threat.TtiS < 12.0f)
			{
				Warn = FLinearColor(1.0f, 0.55f, 0.2f, 1.0f);  // close
			}
			FString TtiStr;
			if (Threat.TtiS > 0.0f)
			{
				TtiStr = FString::Printf(TEXT("   TTI %.0f s"), Threat.TtiS);
			}
			const FString Line1 =
			    FString::Printf(TEXT("THREAT  %.1f KM   CLOSING %.0f m/s%s"),
			                    Threat.RangeM / 1000.0f, Threat.ClosingMps, *TtiStr);
			Label(Out, Layer + 5, Geometry, FVector2D(Center.X - 250, 60), FVector2D(520, 26), Line1,
			      FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 15), Warn);
		}

		// Off-screen threat chevron pointing where to turn.
		const USeaGunsightWidget::FOffscreenCue Cue = Data->OffscreenThreat();
		if (Cue.bActive)
		{
			const FVector2D Fwd(FMath::Cos(Cue.AngleRad), FMath::Sin(Cue.AngleRad));
			const FVector2D Side(-Fwd.Y, Fwd.X);
			const FLinearColor C(1.0f, 0.35f, 0.30f, 1.0f);
			const FVector2D Tip = Cue.EdgeLocal + Fwd * 16.0;
			const FVector2D BackL = Tip - Fwd * 22.0 + Side * 13.0;
			const FVector2D BackR = Tip - Fwd * 22.0 - Side * 13.0;
			Line(Out, Layer + 5, Geo, Tip, BackL, C, 2.4f);
			Line(Out, Layer + 5, Geo, Tip, BackR, C, 2.4f);
			Line(Out, Layer + 5, Geo, BackL, BackR, C, 2.4f);
			FSlateDrawElement::MakeText(
			    Out, Layer + 5,
			    Geometry.ToPaintGeometry(
			        FVector2f(120, 16),
			        FSlateLayoutTransform(FVector2f(Cue.EdgeLocal.X - Fwd.X * 40.0f - 30.0f,
			                                        Cue.EdgeLocal.Y - Fwd.Y * 40.0f - 8.0f))),
			    FString::Printf(TEXT("THREAT %.1fKM"), Cue.RangeM / 1000.0f), Font,
			    ESlateDrawEffect::None, C);
		}

		return Layer + 6;
	}

private:
	TWeakObjectPtr<const USeaGunsightWidget> Owner;
};

TSharedRef<SWidget> USeaGunsightHost::RebuildWidget()
{
	TSharedRef<SSeaGunsight> S = SNew(SSeaGunsight).Owner(Owner);
	MySight = S;
	return S;
}

void USeaGunsightHost::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MySight.Reset();
}

TSharedRef<SWidget> USeaGunsightWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		USeaGunsightHost* H = WidgetTree->ConstructWidget<USeaGunsightHost>(USeaGunsightHost::StaticClass());
		H->Owner = this;
		if (UCanvasPanelSlot* Slot = Root->AddChildToCanvas(H))
		{
			// Fill the viewport so the painter's geometry IS the screen: the
			// reticle centers and target markers land at their viewport coords.
			Slot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
			Slot->SetOffsets(FMargin(0.0f));
		}
		Host = H;
		WidgetTree->RootWidget = Root;
	}
	return Super::RebuildWidget();
}

void USeaGunsightWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GI->GetSubsystem<USeaNetSubsystem>())
		{
			Net->OnEngagementEvent.AddDynamic(this, &USeaGunsightWidget::HandleKillEvent);
		}
	}
}

void USeaGunsightWidget::HandleKillEvent(const FSeaEngagementEvent& Event)
{
	if (Event.Kind == 7)  // kRoundStart — drop the previous wave's kill marks.
	{
		KillWorld.Reset();
		LastTargetWorldCm.Reset();
		return;
	}
	if (Event.Kind == 2)  // kTargetDestroyed — mark the kill at the target's last pos.
	{
		if (const FVector* W = LastTargetWorldCm.Find(Event.SubjectId))
		{
			KillWorld.Add({*W, 0.0f});
		}
	}
}

void USeaGunsightWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	PulsePhaseS += InDeltaTime;
	CachedMarkers.Reset();
	CachedKillMarks.Reset();

	APlayerController* PC = GetOwningPlayer();
	USeaNetSubsystem* Net = nullptr;
	if (const UGameInstance* GI = GetGameInstance())
	{
		Net = GI->GetSubsystem<USeaNetSubsystem>();
	}
	bVisibleNow = PC != nullptr && Net != nullptr && Net->IsWelcomed() &&
	              Net->GetRole() != ESeaRole::Observer;
	if (Host != nullptr)
	{
		Host->SetVisibility(bVisibleNow ? ESlateVisibility::HitTestInvisible
		                                : ESlateVisibility::Collapsed);
	}
	if (!bVisibleNow)
	{
		return;
	}

	const float DpiScale = UWidgetLayoutLibrary::GetViewportScale(this);
	const int32 Designated = Net->GetDesignatedTrack();
	CachedLeadError = Net->GetLeadError();

	// Project an ENU-meters point (relative to the ship at the logical origin)
	// to viewport-local pixels. Returns whether it landed in front of the eye.
	auto ProjectEnu = [&](const FVector& EnuMeters, FVector2D& OutLocal) -> bool
	{
		const FVector World =
		    FVector(EnuMeters.Y * 100.0, EnuMeters.X * 100.0, EnuMeters.Z * 100.0) +
		    SeaWorldFrame::Origin;
		FVector2D Screen;
		if (!PC->ProjectWorldLocationToScreen(World, Screen, /*bPlayerViewportRelative=*/false))
		{
			return false;
		}
		OutLocal = FVector2D(Screen.X / DpiScale, Screen.Y / DpiScale);
		return true;
	};
	// UE-cm point (relative to logical origin) -> viewport-local pixels.
	auto ProjectUeCm = [&](const FVector& UeCm, FVector2D& OutLocal) -> bool
	{
		FVector2D Screen;
		if (!PC->ProjectWorldLocationToScreen(UeCm + SeaWorldFrame::Origin, Screen,
		                                      /*bPlayerViewportRelative=*/false))
		{
			return false;
		}
		OutLocal = FVector2D(Screen.X / DpiScale, Screen.Y / DpiScale);
		return true;
	};
	// Salvo dispersion footprint at an ENU point, projected to a pixel radius:
	// the pattern is angular (mrad), so it grows with range — the whole reason a
	// distant shot is a gamble. Probe by offsetting one radius straight up.
	auto FootprintPixels = [&](const FVector& PointEnu, float DispMrad,
	                           const FVector2D& LocalPx) -> float
	{
		const float RadiusM = DispMrad * 1e-3f * static_cast<float>(PointEnu.Size());
		FVector2D Edge;
		if (RadiusM > 0.1f && ProjectEnu(PointEnu + FVector(0.0, 0.0, RadiusM), Edge))
		{
			return FVector2D::Distance(LocalPx, Edge);
		}
		return 0.0f;
	};

	TArray<FSeaEntityState> Entities;
	Net->SampleEntities(Entities);
	bool bHaveTarget = false;
	FVector TargetPosUeCm = FVector::ZeroVector;
	FVector TargetVelUeCm = FVector::ZeroVector;
	for (const FSeaEntityState& E : Entities)
	{
		if (E.Kind != ESeaEntityKind::Target)
		{
			continue;
		}
		// A live target (state 0); a destroyed one (state 1) is wreckage.
		if (E.State == 0 && !bHaveTarget)
		{
			bHaveTarget = true;
			TargetPosUeCm = E.Position;
			TargetVelUeCm = E.Velocity;
		}
		const FVector World = E.Position + SeaWorldFrame::Origin;
		LastTargetWorldCm.Add(E.Id, World);  // remembered so a kill can mark the spot
		FVector2D Screen;
		// Only mark targets in front of the camera and on screen.
		if (!PC->ProjectWorldLocationToScreen(World, Screen, /*bPlayerViewportRelative=*/false))
		{
			continue;
		}
		FMarker M;
		M.Local = FVector2D(Screen.X / DpiScale, Screen.Y / DpiScale);
		M.RangeM = E.Position.Size() / 100.0f;  // slant range from ownship (logical origin)
		M.Kind = static_cast<uint8>(E.Kind);
		M.bDesignated = (E.Id == Designated && Designated != 0);
		CachedMarkers.Add(M);
	}

	// Age + project the world-anchored kill marks (brief "X KILL", ~1.6 s).
	for (int32 i = 0; i < KillWorld.Num();)
	{
		KillWorld[i].AgeS += InDeltaTime;
		if (KillWorld[i].AgeS > 1.6f)
		{
			KillWorld.RemoveAt(i);
			continue;
		}
		FVector2D Screen;
		FKillMark KM;
		KM.AgeS = KillWorld[i].AgeS;
		KM.bOnScreen =
		    PC->ProjectWorldLocationToScreen(KillWorld[i].WorldCm, Screen, /*viewportRel=*/false);
		if (KM.bOnScreen)
		{
			KM.Local = FVector2D(Screen.X / DpiScale, Screen.Y / DpiScale);
		}
		CachedKillMarks.Add(KM);
		++i;
	}

	// --- Fire-control aim aids -------------------------------------------------
	Impact = FAimPoint{};
	Lead = FAimPoint{};
	ThreatReadout = FThreatReadout{};
	bSolution = false;

	const ASeaGunnerPawn* Gunner = Cast<ASeaGunnerPawn>(PC->GetPawn());
	if (Gunner != nullptr)
	{
		// Steady mean wind in ENU m/s (the gust is unknowable to the aim).
		const FVector WindCms = Net->GetWeather().WindCms;
		const FVector WindEnu(WindCms.Y / 100.0, WindCms.X / 100.0, WindCms.Z / 100.0);
		// Launch platform pose: the bore starts on the ship and inherits its
		// velocity, so the pipper/lead stay honest while the player maneuvers.
		FVector ShipPosEnu = FVector::ZeroVector;
		FVector ShipVelEnu = FVector::ZeroVector;
		FSeaEntityState ShipState;
		if (Net->GetOwnShip(ShipState))
		{
			ShipPosEnu = FVector(ShipState.Position.Y / 100.0, ShipState.Position.X / 100.0,
			                     ShipState.Position.Z / 100.0);
			ShipVelEnu = FVector(ShipState.Velocity.Y / 100.0, ShipState.Velocity.X / 100.0,
			                     ShipState.Velocity.Z / 100.0);
		}
		const double Az = Gunner->AimAzimuthDeg();
		const double El = Gunner->AimElevationDeg();
		FVector2D L;
		if (bHaveTarget)
		{
			// 3D intercept solution: where the salvo passes nearest the threat,
			// and where the threat will be then. Align them to score a hit.
			const FVector TPosEnu(TargetPosUeCm.Y / 100.0, TargetPosUeCm.X / 100.0,
			                      TargetPosUeCm.Z / 100.0);
			const FVector TVelEnu(TargetVelUeCm.Y / 100.0, TargetVelUeCm.X / 100.0,
			                      TargetVelUeCm.Z / 100.0);
			const SeaBallistics::FFireAid Aid =
			    SeaBallistics::ComputeFireAid(Az, El, WindEnu, TPosEnu, TVelEnu, ShipPosEnu, ShipVelEnu);
			if (Aid.bValid)
			{
				Impact.bValid = true;
				Impact.bOnScreen = ProjectEnu(Aid.PipperEnu, L);
				Impact.Local = L;
				Impact.FootprintPx =
				    FootprintPixels(Aid.PipperEnu, Net->GetOrderDispersionMrad(), Impact.Local);
				Lead.bValid = true;
				Lead.bOnScreen = ProjectEnu(Aid.LeadEnu, L);
				Lead.Local = L;
				// Within the proximity-fuze radius (sim ~12 m) reads as a firing
				// solution — exactly the adjudication the server will run.
				if (Aid.MissM < 16.0f && Impact.bOnScreen)
				{
					bSolution = true;
				}
			}
		}
		else
		{
			// No threat up: still show where the bore's salvo would splash.
			const SeaBallistics::FImpact Pred = SeaBallistics::PredictImpact(Az, El, WindEnu, ShipPosEnu, ShipVelEnu);
			if (Pred.bValid)
			{
				Impact.bValid = true;
				Impact.bOnScreen = ProjectEnu(Pred.EnuMeters, L);
				Impact.Local = L;
				Impact.FootprintPx =
				    FootprintPixels(Pred.EnuMeters, Net->GetOrderDispersionMrad(), Impact.Local);
			}
		}
	}

	if (bHaveTarget)
	{
		const float RangeM = TargetPosUeCm.Size() / 100.0f;
		// Closing speed = velocity component toward the ship (origin).
		const FVector ToShip = (-TargetPosUeCm).GetSafeNormal();
		const float ClosingMps = FVector::DotProduct(TargetVelUeCm, ToShip) / 100.0f;
		ThreatReadout.bValid = true;
		ThreatReadout.RangeM = RangeM;
		ThreatReadout.ClosingMps = ClosingMps;
		ThreatReadout.TtiS = ClosingMps > 1.0f ? RangeM / ClosingMps : 0.0f;
		FVector2D ThreatLocal;
		ThreatReadout.bOnScreen = ProjectUeCm(TargetPosUeCm, ThreatLocal);
		ThreatReadout.Local = ThreatLocal;
	}

	// Off-screen threat cue: a chevron at the screen edge pointing at the threat
	// when it is out of view — the ASM can come from any bearing, so the player
	// needs to know which way to turn.
	OffscreenCue = FOffscreenCue{};
	if (bHaveTarget)
	{
		const FVector2D ViewSize = MyGeometry.GetLocalSize();
		const FVector2D ViewCenter = ViewSize * 0.5;
		const float Margin = 72.0f;
		const bool bInView = ThreatReadout.bOnScreen && ThreatReadout.Local.X > Margin &&
		                     ThreatReadout.Local.X < ViewSize.X - Margin &&
		                     ThreatReadout.Local.Y > Margin && ThreatReadout.Local.Y < ViewSize.Y - Margin;
		if (!bInView)
		{
			if (APlayerCameraManager* Cam = PC->PlayerCameraManager)
			{
				const FRotationMatrix Basis(Cam->GetCameraRotation());
				const FVector To = (TargetPosUeCm + SeaWorldFrame::Origin) - Cam->GetCameraLocation();
				const float R = FVector::DotProduct(To, Basis.GetUnitAxis(EAxis::Y));
				const float U = FVector::DotProduct(To, Basis.GetUnitAxis(EAxis::Z));
				FVector2D Dir(R, -U);  // screen space: +x right, +y down
				if (Dir.Normalize() && ViewCenter.X > Margin && ViewCenter.Y > Margin)
				{
					const float Scale = FMath::Min((ViewCenter.X - Margin) / FMath::Max(FMath::Abs(Dir.X), 1e-3f),
					                                (ViewCenter.Y - Margin) / FMath::Max(FMath::Abs(Dir.Y), 1e-3f));
					OffscreenCue.bActive = true;
					OffscreenCue.EdgeLocal = ViewCenter + Dir * Scale;
					OffscreenCue.AngleRad = FMath::Atan2(Dir.Y, Dir.X);
					OffscreenCue.RangeM = ThreatReadout.RangeM;
				}
			}
		}
	}
}
