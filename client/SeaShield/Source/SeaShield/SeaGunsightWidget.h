#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "CoreMinimal.h"

#include "SeaNetSubsystem.h"

#include "SeaGunsightWidget.generated.h"

// Heads-up gun sight for the first-person director: a center reticle plus a
// designator box around every tracked target/rocket projected into the view,
// with slant range — so a target that is only a few pixels at battle range is
// still findable and the player has an aim point. World->screen projection runs
// on the game thread (NativeTick, needs the player controller); the SLeafWidget
// painter just draws the cached markers (UE 5.7 NativePaint-on-screen workaround
// shared with the PPI/console).
UCLASS()
class SEASHIELD_API USeaGunsightWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	struct FMarker
	{
		FVector2D Local = FVector2D::ZeroVector;  // viewport-local (DPI-adjusted)
		float RangeM = 0.0f;
		uint8 Kind = 0;     // ESeaEntityKind
		bool bDesignated = false;
	};
	const TArray<FMarker>& Markers() const { return CachedMarkers; }

	// Fire-control aim aids (the heart of unguided gunnery): where the current
	// bore's salvo is predicted to land (Impact), and where the threat will be
	// after that flight time (Lead). Put the pipper on the lead ghost to score a
	// hit; bSolution latches when they coincide.
	struct FAimPoint
	{
		FVector2D Local = FVector2D::ZeroVector;
		bool bValid = false;
		bool bOnScreen = false;
		float FootprintPx = 0.0f;  // salvo dispersion radius at this point, in pixels
	};
	FAimPoint ImpactPoint() const { return Impact; }
	FAimPoint LeadPoint() const { return Lead; }
	bool HasSolution() const { return bSolution; }

	// Threat read-out for the inbound target: slant range, closing speed and
	// time-to-impact at the ship.
	struct FThreatReadout
	{
		bool bValid = false;
		float RangeM = 0.0f;
		float ClosingMps = 0.0f;
		float TtiS = 0.0f;
		FVector2D Local = FVector2D::ZeroVector;  // anchor for the label
		bool bOnScreen = false;
	};
	FThreatReadout Threat() const { return ThreatReadout; }

	// Edge cue pointing at the threat when it is outside the view — so the player
	// knows which way to turn to find an inbound target (it can come from any
	// bearing). EdgeLocal is the clamped screen position, AngleRad the chevron's
	// heading (screen space, 0 = +X right, CW).
	struct FOffscreenCue
	{
		bool bActive = false;
		FVector2D EdgeLocal = FVector2D::ZeroVector;
		float AngleRad = 0.0f;
		float RangeM = 0.0f;
	};
	FOffscreenCue OffscreenThreat() const { return OffscreenCue; }

	// Lead-error of the last salvo (from USeaNetSubsystem), shown briefly so the
	// player learns the correction: how far / which way the threat slipped out of
	// the committed solution.
	USeaNetSubsystem::FSeaLeadError LeadError() const { return CachedLeadError; }

	// 0..1 pulse for attention-getting elements (threat box, SOLUTION cue).
	float Pulse() const { return 0.5f + 0.5f * FMath::Sin(PulsePhaseS * 6.2831853f); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor ReticleColor = FLinearColor(0.55f, 1.0f, 0.62f, 0.9f);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor TargetColor = FLinearColor(1.0f, 0.45f, 0.3f, 0.95f);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor ImpactColor = FLinearColor(1.0f, 0.78f, 0.20f, 0.95f);  // amber: not solved
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor SolutionColor = FLinearColor(0.35f, 1.0f, 0.45f, 1.0f);  // green: solved
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor LeadColor = FLinearColor(0.35f, 0.85f, 1.0f, 0.95f);  // cyan: aim here

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	TArray<FMarker> CachedMarkers;
	FAimPoint Impact;
	FAimPoint Lead;
	FThreatReadout ThreatReadout;
	FOffscreenCue OffscreenCue;
	USeaNetSubsystem::FSeaLeadError CachedLeadError;
	bool bSolution = false;
	bool bVisibleNow = false;
	float PulsePhaseS = 0.0f;

	UPROPERTY()
	TObjectPtr<class USeaGunsightHost> Host;
};

UCLASS()
class SEASHIELD_API USeaGunsightHost : public UWidget
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<const USeaGunsightWidget> Owner;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	TSharedPtr<SWidget> MySight;
};
