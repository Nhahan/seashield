#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "CoreMinimal.h"

#include "SeaNetSubsystem.h"

#include "SeaAfterActionWidget.generated.h"

// After-action review (charter §5.8 / §7): a centered summary that appears when
// the engagement ends (kEngagementEnd) — rockets fired, detonations, kills,
// per-salvo effectiveness, miss statistics and the weather it was fought in.
// Pure C++ Slate (same SLeafWidget pattern and rationale as the PPI/console).
// Every figure comes from the reliable engagement-event stream, so the review
// is the server's record of what happened, not a client guess.
UCLASS()
class SEASHIELD_API USeaAfterActionWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor TextColor = FLinearColor(0.62f, 1.0f, 0.78f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SeaShield")
	FLinearColor DimColor = FLinearColor(0.40f, 0.72f, 0.58f);

	const TArray<FString>& Lines() const { return CachedLines; }

	UFUNCTION()
	void HandleEngagementEvent(const FSeaEngagementEvent& Event);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	TArray<FString> CachedLines;

	int32 Launches = 0;
	int32 Bursts = 0;
	int32 Splashes = 0;
	int32 Kills = 0;
	float BestMissM = -1.0f;
	float LastMissM = -1.0f;
	int64 FirstTick = -1;
	int64 LastTick = 0;
	bool bEngagementEnded = false;

	UPROPERTY()
	TObjectPtr<class USeaAarPanelHost> PanelHost;
};

UCLASS()
class SEASHIELD_API USeaAarPanelHost : public UWidget
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<const USeaAfterActionWidget> Owner;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	TSharedPtr<SWidget> MyPanel;
};
