#include "SeaAfterActionWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Fonts/SlateFontInfo.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

namespace {
constexpr float kPanelWidth = 560.0f;
constexpr float kPanelHeight = 320.0f;
constexpr float kRowHeight = 26.0f;
constexpr float kPadX = 28.0f;
constexpr float kPadY = 22.0f;
}  // namespace

// Centered AAR card painter.
class SSeaAarPanel final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSeaAarPanel) {}
	SLATE_ARGUMENT(TWeakObjectPtr<const USeaAfterActionWidget>, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;
		SetCanTick(false);
		ForceVolatile(true);
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return FVector2D(kPanelWidth, kPanelHeight);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	                      const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	                      int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override
	{
		const USeaAfterActionWidget* Data = Owner.Get();
		const FVector2D Size = AllottedGeometry.GetLocalSize();
		if (Data == nullptr || Size.X <= 0.0f)
		{
			return LayerId;
		}
		static const FSlateColorBrush Bg(FLinearColor::White);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), &Bg,
		                           ESlateDrawEffect::None, FLinearColor(0.01f, 0.02f, 0.012f, 0.92f));
		++LayerId;
		const TArray<FVector2D> Frame = {FVector2D(2, 2), FVector2D(Size.X - 2, 2),
		                                 FVector2D(Size.X - 2, Size.Y - 2),
		                                 FVector2D(2, Size.Y - 2), FVector2D(2, 2)};
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		                             Frame, ESlateDrawEffect::None, Data->DimColor, true, 1.4f);

		const FSlateFontInfo Title = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 17);
		const FSlateFontInfo Body = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 13);
		float Y = kPadY;
		const TArray<FString>& Lines = Data->Lines();
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			FSlateDrawElement::MakeText(
			    OutDrawElements, LayerId + 1,
			    AllottedGeometry.ToPaintGeometry(FVector2f(Size.X - 2.0f * kPadX, kRowHeight),
			                                     FSlateLayoutTransform(FVector2f(kPadX, Y))),
			    Lines[i], i == 0 ? Title : Body, ESlateDrawEffect::None,
			    i == 0 ? Data->TextColor : (i == 1 ? Data->DimColor : Data->TextColor));
			Y += i == 0 ? kRowHeight + 8.0f : kRowHeight;
			if (i == 0)
			{
				FSlateDrawElement::MakeLines(
				    OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
				    {FVector2D(kPadX, Y - 6.0f), FVector2D(Size.X - kPadX, Y - 6.0f)},
				    ESlateDrawEffect::None, Data->DimColor, true, 1.0f);
			}
		}
		return LayerId + 2;
	}

private:
	TWeakObjectPtr<const USeaAfterActionWidget> Owner;
};

TSharedRef<SWidget> USeaAarPanelHost::RebuildWidget()
{
	TSharedRef<SSeaAarPanel> Panel = SNew(SSeaAarPanel).Owner(Owner);
	MyPanel = Panel;
	return Panel;
}

void USeaAarPanelHost::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyPanel.Reset();
}

TSharedRef<SWidget> USeaAfterActionWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		USeaAarPanelHost* Host =
		    WidgetTree->ConstructWidget<USeaAarPanelHost>(USeaAarPanelHost::StaticClass());
		Host->Owner = this;
		if (UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Host))
		{
			Slot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
			Slot->SetAlignment(FVector2D(0.5f, 0.5f));
			Slot->SetPosition(FVector2D(0.0f, 0.0f));
			Slot->SetSize(FVector2D(kPanelWidth, kPanelHeight));
		}
		PanelHost = Host;
		WidgetTree->RootWidget = Root;
	}
	return Super::RebuildWidget();
}

void USeaAfterActionWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>())
		{
			Net->OnEngagementEvent.AddDynamic(this, &USeaAfterActionWidget::HandleEngagementEvent);
		}
	}
}

void USeaAfterActionWidget::HandleEngagementEvent(const FSeaEngagementEvent& Event)
{
	LastTick = Event.Tick;
	switch (Event.Kind)
	{
	case 0:  // kLaunch
		if (FirstTick < 0) { FirstTick = Event.Tick; }
		++Launches;
		break;
	case 1:  // kRocketResolved
		Event.bDetonated ? ++Bursts : ++Splashes;
		LastMissM = Event.MissDistanceM;
		if (BestMissM < 0.0f || Event.MissDistanceM < BestMissM) { BestMissM = Event.MissDistanceM; }
		break;
	case 2:  // kTargetDestroyed
		++Kills;
		break;
	case 3:  // kEngagementEnd
		bEngagementEnded = true;
		break;
	default:
		break;
	}
}

void USeaAfterActionWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	USeaNetSubsystem* Net = nullptr;
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		Net = GameInstance->GetSubsystem<USeaNetSubsystem>();
	}
	// Shown when the engagement ends; -SeaShowAAR forces it for capture.
	const bool bForce = FParse::Param(FCommandLine::Get(), TEXT("SeaShowAAR"));
	const bool bShow = (bEngagementEnded || bForce) && Net != nullptr && Launches > 0;
	if (PanelHost != nullptr)
	{
		PanelHost->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible
		                               : ESlateVisibility::Collapsed);
	}
	if (!bShow)
	{
		return;
	}

	const int32 Resolved = Bursts + Splashes;
	const float KillPct = Launches > 0 ? 100.0f * static_cast<float>(Kills) / Launches : 0.0f;
	const float DurationS = FirstTick >= 0 ? static_cast<float>(LastTick - FirstTick) / 60.0f : 0.0f;
	const FSeaWeather Weather = Net->GetWeather();
	double WindBearing =
	    FMath::RadiansToDegrees(FMath::Atan2(Weather.WindCms.Y, Weather.WindCms.X));
	if (WindBearing < 0.0)
	{
		WindBearing += 360.0;
	}

	const FSeaGameState GS = Net->GetGameState();
	CachedLines.Reset();
	CachedLines.Add(TEXT("AFTER-ACTION REVIEW"));
	CachedLines.Add(bEngagementEnded ? TEXT("ENGAGEMENT COMPLETE") : TEXT("ENGAGEMENT IN PROGRESS"));
	if (GS.Wave > 0)  // Survival game run: headline the score.
	{
		CachedLines.Add(FString::Printf(TEXT("FINAL SCORE       %d"), GS.Score));
		CachedLines.Add(FString::Printf(TEXT("WAVE REACHED      %d   BEST STREAK x%d"), GS.Wave,
		                                GS.BestStreak));
	}
	CachedLines.Add(FString::Printf(TEXT("ROCKETS FIRED     %d"), Launches));
	CachedLines.Add(FString::Printf(TEXT("RESOLVED          %d   (BURST %d / SPLASH %d)"), Resolved,
	                                Bursts, Splashes));
	CachedLines.Add(FString::Printf(TEXT("TARGETS KILLED    %d"), Kills));
	CachedLines.Add(FString::Printf(TEXT("EFFECTIVENESS     %.0f%%  (kills / rockets)"), KillPct));
	if (BestMissM >= 0.0f)
	{
		CachedLines.Add(FString::Printf(TEXT("BEST / LAST MISS  %.0fM / %.0fM"), BestMissM, LastMissM));
	}
	else
	{
		CachedLines.Add(TEXT("BEST / LAST MISS  --"));
	}
	CachedLines.Add(FString::Printf(TEXT("CONDITIONS        WIND %.1f M/S @ %03d  RAIN %.0f%%"),
	                                Weather.WindCms.Size() / 100.0,
	                                static_cast<int32>(FMath::RoundToInt(WindBearing)),
	                                Weather.RainIntensity * 100.0f));
	CachedLines.Add(FString::Printf(TEXT("DURATION          %.0f S"), DurationS));
}
