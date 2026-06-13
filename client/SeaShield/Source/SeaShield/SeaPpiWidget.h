#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "CoreMinimal.h"

#include "SeaNetSubsystem.h"

#include "SeaPpiWidget.generated.h"

class SSeaPpiScope;

// Radar PPI scope (charter §7 화면 ② — NTDS-style symbology), drawn entirely
// as vector graphics: range rings, bearing ticks, the rotating sweep, track
// symbols with their σ ring, and the designated track's PIP + dispersion
// circle from the streamed fire solution. Everything on screen is server
// data — the scope plots the ESTIMATE stream, never the truth (§5.5).
//
// Structure: USeaPpiWidget owns the data (entity cache, sweep clock,
// selection) and hosts an SSeaPpiScope leaf widget that does the painting.
// The painting deliberately does NOT live in UUserWidget::NativePaint: on
// UE 5.7 the SObjectWidget-routed paint never reached the screen (verified
// against a stock UImage probe rendering fine in the same tree), while a
// plain SLeafWidget in the tree paints reliably.
UCLASS()
class SEASHIELD_API USeaPpiWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// Scope radius in simulation meters (the outer ring).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	float DisplayRangeM = 15000.0f;

	// Sweep rotation period — mirrors the radar's scan_period (welcome data
	// carries no radar params; default matches the scenario default 2 s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	float SweepPeriodS = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor ScopeColor = FLinearColor(0.05f, 0.32f, 0.10f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor TrackColor = FLinearColor(0.45f, 1.0f, 0.55f);

	UPROPERTY(BlueprintReadWrite, Category = "SeaShield")
	int32 SelectedTrackId = 0;

	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	void SelectTrack(int32 TrackId) { SelectedTrackId = TrackId; }

	// Paint-side data access (SSeaPpiScope).
	const TArray<FSeaEntityState>& Entities() const { return CachedEntities; }
	float SweepAngle() const { return SweepAngleRad; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	float SweepAngleRad = 0.0f;
	TArray<FSeaEntityState> CachedEntities;  // Refreshed in NativeTick.

	// The scope leaf's host — gated on connection/role in NativeTick. The
	// OUTER widget always stays visible so this tick keeps running (collapsed
	// UserWidgets stop ticking and could never un-collapse themselves).
	UPROPERTY()
	TObjectPtr<class USeaPpiScopeHost> ScopeHost;
};

// UWidget shim that mounts the SSeaPpiScope leaf into the widget tree.
UCLASS()
class SEASHIELD_API USeaPpiScopeHost : public UWidget
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<USeaPpiWidget> Owner;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	TSharedPtr<SWidget> MyScope;
};
