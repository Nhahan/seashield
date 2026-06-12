#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"

#include "SeaNetSubsystem.h"

#include "SeaPpiWidget.generated.h"

// Radar PPI scope (charter §7 화면 ② — NTDS-style symbology), drawn entirely
// in NativePaint as vector graphics: range rings, bearing ticks, the rotating
// sweep, track symbols with their σ ring, and the designated track's PIP +
// dispersion circle from the streamed fire solution. Everything on screen is
// server data — the scope plots the ESTIMATE stream, never the truth (§5.5).
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

	// The nearest track symbol within GrabRadiusPx of a scope-local point —
	// the click-to-designate path for the weapons console. 0 if none.
	UFUNCTION(BlueprintCallable, Category = "SeaShield")
	int32 TrackAtPosition(const FVector2D& LocalPoint, float GrabRadiusPx = 24.0f) const;

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	                          const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	                          int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                          bool bParentEnabled) const override;

private:
	// Sim ENU (meters, origin = own ship) -> scope-local pixels. North-up
	// display: +north is up, +east is right.
	FVector2D ToScope(const FGeometry& Geometry, double EastM, double NorthM) const;

	float SweepAngleRad = 0.0f;
	TArray<FSeaEntityState> CachedEntities;  // Refreshed in NativeTick.
};
