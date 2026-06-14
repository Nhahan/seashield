#include "SeaGameHudWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

#include "SeaNetSubsystem.h"

// Fullscreen survival-game HUD painter: scoreboard top-left, life pips, and the
// big transient center banner.
class SSeaGameHud final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSeaGameHud) {}
	SLATE_ARGUMENT(TWeakObjectPtr<const USeaGameHudWidget>, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;
		SetCanTick(false);
		ForceVolatile(true);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(8.0, 8.0); }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Cull,
	                      FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style,
	                      bool bEnabled) const override
	{
		const USeaGameHudWidget* Data = Owner.Get();
		if (Data == nullptr)
		{
			return Layer;
		}
		const USeaGameHudWidget::FState& S = Data->State();
		const FVector2D Size = Geometry.GetLocalSize();
		const FPaintGeometry Geo = Geometry.ToPaintGeometry();
		static const FSlateColorBrush Box(FLinearColor::White);

		// Full-screen confirm flashes (drawn under the HUD): green on a kill, red
		// on a ship hit — so the payoff/penalty is unmistakable.
		if (S.KillFlash > 0.01f)
		{
			FSlateDrawElement::MakeBox(Out, Layer, Geo, &Box, ESlateDrawEffect::None,
			                           FLinearColor(0.25f, 1.0f, 0.4f, 0.16f * S.KillFlash));
		}
		if (S.HitFlash > 0.01f)
		{
			FSlateDrawElement::MakeBox(Out, Layer, Geo, &Box, ESlateDrawEffect::None,
			                           FLinearColor(1.0f, 0.15f, 0.12f, 0.34f * S.HitFlash));
		}

		const FSlateFontInfo Big = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 26);
		const FSlateFontInfo Mid = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 16);
		const FLinearColor Ink(0.80f, 0.95f, 0.85f, 0.95f);
		const FLinearColor Dim(0.55f, 0.70f, 0.62f, 0.85f);

		auto Text = [&](const FString& Str, const FSlateFontInfo& Font, float X, float Y,
		                const FLinearColor& Col, float W = 420.0f)
		{
			FSlateDrawElement::MakeText(
			    Out, Layer + 2,
			    Geometry.ToPaintGeometry(FVector2f(W, Font.Size + 6.0f),
			                             FSlateLayoutTransform(FVector2f(X, Y))),
			    Str, Font, ESlateDrawEffect::None, Col);
		};

		// Scoreboard panel, top-left.
		FSlateDrawElement::MakeBox(
		    Out, Layer,
		    Geometry.ToPaintGeometry(FVector2f(270, 116), FSlateLayoutTransform(FVector2f(24, 22))),
		    &Box, ESlateDrawEffect::None, FLinearColor(0.01f, 0.02f, 0.012f, 0.55f));
		Text(FString::Printf(TEXT("WAVE %d"), S.Wave), Big, 36, 28, Ink, 220.0f);
		Text(FString::Printf(TEXT("SPLASHED  %d"), S.Kills), Mid, 38, 70, Dim, 220.0f);
		// Nearest miss this wave — makes "unguided is hard" a measured number.
		if (S.BestMissM > 0.0f)
		{
			Text(FString::Printf(TEXT("NEAREST MISS  %.0f m"), S.BestMissM), Mid, 38, 96,
			     FLinearColor(1.0f, 0.7f, 0.3f, 0.9f), 260.0f);
		}

		// Life pips, top-right.
		const float PipR = 18.0f;
		const float PipGap = 10.0f;
		const float PipsW = S.MaxLives * PipR + (S.MaxLives - 1) * PipGap;
		float Px = Size.X - 36.0f - PipsW;
		Text(TEXT("SHIP"), Mid, Px - 4, 24, Dim, 80.0f);
		for (int32 i = 0; i < S.MaxLives; ++i)
		{
			const bool bAlive = i < S.Lives;
			const FLinearColor Col = bAlive ? FLinearColor(0.35f, 1.0f, 0.45f, 0.95f)
			                                : FLinearColor(0.5f, 0.12f, 0.10f, 0.7f);
			FSlateDrawElement::MakeBox(
			    Out, Layer + 1,
			    Geometry.ToPaintGeometry(FVector2f(PipR, PipR),
			                             FSlateLayoutTransform(FVector2f(Px, 50))),
			    &Box, ESlateDrawEffect::None, Col);
			Px += PipR + PipGap;
		}

		// Center transient banner.
		if (!S.Banner.IsEmpty() && S.BannerAlpha > 0.01f)
		{
			const FSlateFontInfo Banner = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 40);
			FLinearColor Col = S.BannerColor;
			Col.A *= S.BannerAlpha;
			// Dark plate so the banner reads even over the green kill flash.
			FSlateDrawElement::MakeBox(
			    Out, Layer + 2,
			    Geometry.ToPaintGeometry(FVector2f(620, 62),
			                             FSlateLayoutTransform(FVector2f(Size.X * 0.5f - 310.0f,
			                                                             Size.Y * 0.32f - 6.0f))),
			    &Box, ESlateDrawEffect::None, FLinearColor(0.0f, 0.02f, 0.01f, 0.5f * S.BannerAlpha));
			FSlateDrawElement::MakeText(
			    Out, Layer + 3,
			    Geometry.ToPaintGeometry(FVector2f(900, 56),
			                             FSlateLayoutTransform(FVector2f(Size.X * 0.5f - 300.0f,
			                                                             Size.Y * 0.32f))),
			    S.Banner, Banner, ESlateDrawEffect::None, Col);
		}
		// First-wave tutorial line (bottom-center): teaches the lead mechanic —
		// the single biggest thing new players miss (they shoot AT the target).
		if (S.bHint)
		{
			const FSlateFontInfo Hint = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 16);
			const FString Msg =
			    TEXT("LEAD THE TARGET  -  put the amber pipper on the cyan AIM marker  -  fire when GREEN");
			FSlateDrawElement::MakeBox(
			    Out, Layer + 2,
			    Geometry.ToPaintGeometry(FVector2f(820, 30),
			                             FSlateLayoutTransform(FVector2f(Size.X * 0.5f - 410.0f,
			                                                             Size.Y * 0.70f - 4.0f))),
			    &Box, ESlateDrawEffect::None, FLinearColor(0.0f, 0.02f, 0.01f, 0.6f));
			FSlateDrawElement::MakeText(
			    Out, Layer + 3,
			    Geometry.ToPaintGeometry(FVector2f(820, 26),
			                             FSlateLayoutTransform(FVector2f(Size.X * 0.5f - 400.0f,
			                                                             Size.Y * 0.70f))),
			    Msg, Hint, ESlateDrawEffect::None, FLinearColor(0.9f, 0.95f, 0.7f, 0.95f));
		}
		return Layer + 4;
	}

private:
	TWeakObjectPtr<const USeaGameHudWidget> Owner;
};

TSharedRef<SWidget> USeaGameHudHost::RebuildWidget()
{
	TSharedRef<SSeaGameHud> H = SNew(SSeaGameHud).Owner(Owner);
	MyHud = H;
	return H;
}

void USeaGameHudHost::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyHud.Reset();
}

TSharedRef<SWidget> USeaGameHudWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		USeaGameHudHost* H = WidgetTree->ConstructWidget<USeaGameHudHost>(USeaGameHudHost::StaticClass());
		H->Owner = this;
		if (UCanvasPanelSlot* Slot = Root->AddChildToCanvas(H))
		{
			Slot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
			Slot->SetOffsets(FMargin(0.0f));
		}
		Host = H;
		WidgetTree->RootWidget = Root;
	}
	return Super::RebuildWidget();
}

void USeaGameHudWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GI->GetSubsystem<USeaNetSubsystem>())
		{
			Net->OnEngagementEvent.AddDynamic(this, &USeaGameHudWidget::HandleEngagementEvent);
		}
	}
}

void USeaGameHudWidget::HandleEngagementEvent(const FSeaEngagementEvent& Event)
{
	switch (Event.Kind)
	{
	case 1:  // kRocketResolved — track the nearest miss this wave (the thesis:
	         // how far the unguided rounds actually fall from a maneuvering ASM).
		if (!Event.bKilled && Event.MissDistanceM > 0.0f &&
		    (WaveBestMissM < 0.0f || Event.MissDistanceM < WaveBestMissM))
		{
			WaveBestMissM = Event.MissDistanceM;
		}
		break;
	case 7:  // kRoundStart — reset per-wave miss.
		WaveBestMissM = -1.0f;
		break;
	default:
		break;
	}
}

void USeaGameHudWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	USeaNetSubsystem* Net = nullptr;
	if (const UGameInstance* GI = GetGameInstance())
	{
		Net = GI->GetSubsystem<USeaNetSubsystem>();
	}
	const FSeaGameState GS = Net != nullptr ? Net->GetGameState() : FSeaGameState{};
	// The HUD is for the survival game: a plain single-engagement scenario never
	// raises a wave, so it stays hidden there.
	bVisibleNow = Net != nullptr && Net->IsWelcomed() &&
	              Net->GetRole() != ESeaRole::Observer && GS.Wave > 0;
	if (Host != nullptr)
	{
		Host->SetVisibility(bVisibleNow ? ESlateVisibility::HitTestInvisible
		                                : ESlateVisibility::Collapsed);
	}
	if (!bVisibleNow)
	{
		return;
	}

	Cached.Wave = GS.Wave;
	Cached.Kills = GS.Kills;
	Cached.Lives = GS.Lives;
	Cached.MaxLives = GS.MaxLives;
	Cached.bGameOver = GS.bGameOver;

	if (!bInitialized)
	{
		bInitialized = true;
		PrevWave = GS.Wave;
		PrevKills = GS.Kills;
		PrevLeaks = GS.Leaks;
		bPrevGameOver = GS.bGameOver;
	}

	// Raise a banner on the most significant state change this tick.
	auto Raise = [&](const FString& Text, const FLinearColor& Col, float Secs)
	{
		Cached.Banner = Text;
		Cached.BannerColor = Col;
		BannerTimeLeft = Secs;
	};
	if (GS.bGameOver && !bPrevGameOver)
	{
		Raise(TEXT("GAME OVER"), FLinearColor(1.0f, 0.3f, 0.25f, 1.0f), 6.0f);
		HitFlashLeft = 0.8f;
	}
	else if (GS.Leaks > PrevLeaks)
	{
		Raise(TEXT("SHIP HIT!"), FLinearColor(1.0f, 0.3f, 0.25f, 1.0f), 1.8f);
		HitFlashLeft = 0.9f;  // red damage flash
	}
	else if (GS.Kills > PrevKills)
	{
		Raise(TEXT("SPLASH - TARGET DOWN"), FLinearColor(0.4f, 1.0f, 0.5f, 1.0f), 1.6f);
		KillFlashLeft = 0.45f;  // green confirm flash
	}
	else if (GS.Wave > PrevWave)
	{
		Raise(FString::Printf(TEXT("WAVE %d"), GS.Wave), FLinearColor(0.4f, 0.85f, 1.0f, 1.0f), 1.8f);
	}
	PrevWave = GS.Wave;
	PrevKills = GS.Kills;
	PrevLeaks = GS.Leaks;
	bPrevGameOver = GS.bGameOver;

	if (BannerTimeLeft > 0.0f)
	{
		BannerTimeLeft -= InDeltaTime;
		Cached.BannerAlpha = FMath::Clamp(BannerTimeLeft, 0.0f, 1.0f);  // fade out the last second
		if (BannerTimeLeft <= 0.0f)
		{
			Cached.Banner.Reset();
		}
	}

	KillFlashLeft = FMath::Max(0.0f, KillFlashLeft - InDeltaTime);
	HitFlashLeft = FMath::Max(0.0f, HitFlashLeft - InDeltaTime);
	Cached.KillFlash = FMath::Clamp(KillFlashLeft / 0.45f, 0.0f, 1.0f);
	Cached.HitFlash = FMath::Clamp(HitFlashLeft / 0.9f, 0.0f, 1.0f);
	Cached.BestMissM = WaveBestMissM;
	// First-wave tutorial: the lead mechanic is the #1 thing new players miss.
	Cached.bHint = GS.Wave == 1 && GS.Kills == 0 && !GS.bGameOver;
}
