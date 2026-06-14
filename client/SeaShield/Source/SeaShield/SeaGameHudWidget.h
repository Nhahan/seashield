#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "CoreMinimal.h"

#include "SeaNetSubsystem.h"

#include "SeaGameHudWidget.generated.h"

// Survival-game HUD: the persistent scoreboard (wave / kills / lives) plus the
// transient center banners that punctuate the loop — WAVE N, SPLASH (kill),
// SHIP HIT (a leaker), GAME OVER. State comes from USeaNetSubsystem::GetGameState
// (reconstructed from the engagement event stream); banners are raised by
// diffing that state each tick, so no event subscription is needed. Drawing uses
// the SLeafWidget painter (the UE 5.7 NativePaint-on-screen workaround shared
// with the gun sight / PPI / console).
UCLASS()
class SEASHIELD_API USeaGameHudWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	struct FState
	{
		int32 Wave = 0;
		int32 Kills = 0;
		int32 Lives = 3;
		int32 MaxLives = 3;
		int32 Score = 0;
		int32 Streak = 0;
		bool bGameOver = false;
		FString Banner;       // transient center text ("" = none)
		float BannerAlpha = 0.0f;
		FLinearColor BannerColor = FLinearColor::White;
		float KillFlash = 0.0f;   // 0..1 green confirm flash
		float HitFlash = 0.0f;    // 0..1 red damage flash
		bool bHint = false;       // show the first-wave lead tutorial line
		float BestMissM = -1.0f;  // nearest miss this wave (-1 = none yet)
	};
	const FState& State() const { return Cached; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UFUNCTION()
	void HandleEngagementEvent(const FSeaEngagementEvent& Event);

	FState Cached;
	bool bVisibleNow = false;
	float KillFlashLeft = 0.0f;
	float HitFlashLeft = 0.0f;
	float WaveBestMissM = -1.0f;

	// Diff sources for raising banners.
	int32 PrevWave = 0;
	int32 PrevKills = 0;
	int32 PrevLeaks = 0;
	bool bPrevGameOver = false;
	bool bInitialized = false;
	float BannerTimeLeft = 0.0f;

	UPROPERTY()
	TObjectPtr<class USeaGameHudHost> Host;
};

UCLASS()
class SEASHIELD_API USeaGameHudHost : public UWidget
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<const USeaGameHudWidget> Owner;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	TSharedPtr<SWidget> MyHud;
};
