#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "CoreMinimal.h"

#include "SeaNetSubsystem.h"

#include "SeaFireControlPanel.generated.h"

class USeaPpiWidget;

// Weapons-console readout (charter §7 화면 ③): link/role status, the welcome
// weather, the designated track's kinematics, the streamed fire solution
// (PIP range/bearing, ToF, dispersion) and the engagement tally. Pure C++
// vector/text drawing on an SLeafWidget, same pattern (and for the same UE
// 5.7 NativePaint reason) as USeaPpiWidget. Every number on the panel is
// server data — the console displays the ESTIMATE stream, never local truth.
UCLASS()
class SEASHIELD_API USeaFireControlPanel : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor TextColor = FLinearColor(0.62f, 1.0f, 0.78f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor DimColor = FLinearColor(0.40f, 0.72f, 0.58f);

	// Selection source: the panel mirrors the PPI's designated track.
	TWeakObjectPtr<const USeaPpiWidget> PpiRef;

	// Paint-side data (SSeaFcPanel) — rebuilt every NativeTick.
	const TArray<FString>& Lines() const { return CachedLines; }

	UFUNCTION()
	void HandleEngagementEvent(const FSeaEngagementEvent& Event);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	TArray<FString> CachedLines;

	// Engagement tally accumulated from the reliable event stream.
	int32 Launches = 0;
	int32 Splashes = 0;
	int32 Bursts = 0;
	int32 Kills = 0;
	float LastMissM = -1.0f;
	bool bEngagementEnded = false;

	// Same outer-stays-visible pattern as the PPI: the host is what gates.
	UPROPERTY()
	TObjectPtr<class USeaFcPanelHost> PanelHost;
};

// UWidget shim mounting the SSeaFcPanel leaf into the widget tree.
UCLASS()
class SEASHIELD_API USeaFcPanelHost : public UWidget
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<const USeaFireControlPanel> Owner;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	TSharedPtr<SWidget> MyPanel;
};
