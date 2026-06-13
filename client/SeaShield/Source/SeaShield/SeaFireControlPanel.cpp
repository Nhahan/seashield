#include "SeaFireControlPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

#include "SeaPpiWidget.h"

namespace {

constexpr float kPanelWidth = 416.0f;
constexpr float kPanelHeight = 188.0f;
constexpr float kRowHeight = 21.0f;
constexpr float kPadX = 14.0f;
constexpr float kPadY = 12.0f;

FString RoleName(ESeaRole Role)
{
	switch (Role)
	{
	case ESeaRole::Commander: return TEXT("CMD");
	case ESeaRole::Weapons: return TEXT("WEAPONS");
	case ESeaRole::Solo: return TEXT("SOLO");
	case ESeaRole::Observer: break;
	}
	return TEXT("OBS");
}

// UE cm position (x north, y east) -> "RNG xx.xKM BRG bbb".
FString RangeBearing(const FVector& PositionCm)
{
	const double NorthM = PositionCm.X / 100.0;
	const double EastM = PositionCm.Y / 100.0;
	const double RangeKm = FMath::Sqrt(NorthM * NorthM + EastM * EastM) / 1000.0;
	double BearingDeg = FMath::RadiansToDegrees(FMath::Atan2(EastM, NorthM));
	if (BearingDeg < 0.0)
	{
		BearingDeg += 360.0;
	}
	return FString::Printf(TEXT("%5.1fKM BRG %03d"), RangeKm,
	                       static_cast<int32>(FMath::RoundToInt(BearingDeg)));
}

}  // namespace

// The console painter: dark bezel, header rule, then the cached text rows.
class SSeaFcPanel final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSeaFcPanel) {}
	SLATE_ARGUMENT(TWeakObjectPtr<const USeaFireControlPanel>, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;
		SetCanTick(false);
		ForceVolatile(true);  // Live data every frame; never cache the paint.
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
		const USeaFireControlPanel* Data = Owner.Get();
		const FVector2D Size = AllottedGeometry.GetLocalSize();
		if (Data == nullptr || Size.X <= 0.0f)
		{
			return LayerId;
		}

		static const FSlateColorBrush BackgroundBrush(FLinearColor::White);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		                           &BackgroundBrush, ESlateDrawEffect::None,
		                           FLinearColor(0.01f, 0.02f, 0.012f, 0.88f));
		++LayerId;

		// Bezel.
		const TArray<FVector2D> Frame = {
		    FVector2D(1, 1), FVector2D(Size.X - 1, 1), FVector2D(Size.X - 1, Size.Y - 1),
		    FVector2D(1, Size.Y - 1), FVector2D(1, 1)};
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		                             Frame, ESlateDrawEffect::None, Data->DimColor, true, 1.2f);

		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 11);
		const FSlateFontInfo HeaderFont = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 12);
		float Y = kPadY;
		const TArray<FString>& Lines = Data->Lines();
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			FSlateDrawElement::MakeText(
			    OutDrawElements, LayerId + 1,
			    AllottedGeometry.ToPaintGeometry(FVector2f(Size.X - 2.0f * kPadX, kRowHeight),
			                                     FSlateLayoutTransform(FVector2f(kPadX, Y))),
			    Lines[i], i == 0 ? HeaderFont : Font, ESlateDrawEffect::None,
			    i == 0 ? Data->TextColor : (i == 1 ? Data->DimColor : Data->TextColor));
			Y += kRowHeight;
			if (i == 0)
			{
				// Rule under the header.
				FSlateDrawElement::MakeLines(
				    OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
				    {FVector2D(kPadX, Y - 3.0f), FVector2D(Size.X - kPadX, Y - 3.0f)},
				    ESlateDrawEffect::None, Data->DimColor, true, 1.0f);
			}
		}
		return LayerId + 2;
	}

private:
	TWeakObjectPtr<const USeaFireControlPanel> Owner;
};

TSharedRef<SWidget> USeaFcPanelHost::RebuildWidget()
{
	TSharedRef<SSeaFcPanel> Panel = SNew(SSeaFcPanel).Owner(Owner);
	MyPanel = Panel;
	return Panel;
}

void USeaFcPanelHost::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyPanel.Reset();
}

TSharedRef<SWidget> USeaFireControlPanel::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		// Same layout strategy as the PPI: fullscreen UserWidget, the panel
		// placed by its own canvas slot (bottom-right).
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		USeaFcPanelHost* Host =
		    WidgetTree->ConstructWidget<USeaFcPanelHost>(USeaFcPanelHost::StaticClass());
		Host->Owner = this;
		if (UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Host))
		{
			Slot->SetAnchors(FAnchors(1.0f, 1.0f, 1.0f, 1.0f));
			Slot->SetAlignment(FVector2D(1.0f, 1.0f));
			Slot->SetPosition(FVector2D(-24.0f, -24.0f));
			Slot->SetSize(FVector2D(kPanelWidth, kPanelHeight));
		}
		PanelHost = Host;
		WidgetTree->RootWidget = Root;
	}
	return Super::RebuildWidget();
}

void USeaFireControlPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>())
		{
			Net->OnEngagementEvent.AddDynamic(this, &USeaFireControlPanel::HandleEngagementEvent);
		}
	}
}

void USeaFireControlPanel::HandleEngagementEvent(const FSeaEngagementEvent& Event)
{
	switch (Event.Kind)
	{
	case 0:  // kLaunch
		++Launches;
		break;
	case 1:  // kRocketResolved
		Event.bDetonated ? ++Bursts : ++Splashes;
		LastMissM = Event.MissDistanceM;
		if (Event.bKilled)
		{
			++Kills;
		}
		break;
	case 2:  // kTargetDestroyed
		++Kills;
		break;
	case 3:  // kEngagementEnd
		bEngagementEnded = true;
		break;
	default:
		break;  // Track lifecycle shows through the PPI symbology instead.
	}
}

void USeaFireControlPanel::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	USeaNetSubsystem* Net = nullptr;
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		Net = GameInstance->GetSubsystem<USeaNetSubsystem>();
	}
	const bool bShow = Net != nullptr && Net->IsWelcomed() && Net->GetRole() != ESeaRole::Observer;
	if (PanelHost != nullptr)
	{
		PanelHost->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible
		                               : ESlateVisibility::Collapsed);
	}
	if (!bShow)
	{
		return;
	}

	const FSeaWeather Weather = Net->GetWeather();
	const int32 SelectedTrackId = PpiRef.IsValid() ? PpiRef->SelectedTrackId : 0;

	CachedLines.Reset();
	CachedLines.Add(TEXT("FIRE CONTROL"));
	CachedLines.Add(FString::Printf(TEXT("LINK ONLINE   ROLE %s   %s"), *RoleName(Net->GetRole()),
	                                bEngagementEnded ? TEXT("ENGAGEMENT END") : TEXT("")));

	double WindBearing =
	    FMath::RadiansToDegrees(FMath::Atan2(Weather.WindCms.Y, Weather.WindCms.X));
	if (WindBearing < 0.0)
	{
		WindBearing += 360.0;
	}
	CachedLines.Add(FString::Printf(TEXT("WIND %4.1fMPS @ %03d  GUST %.1f  RAIN %3.0f%%"),
	                                Weather.WindCms.Size() / 100.0,
	                                static_cast<int32>(FMath::RoundToInt(WindBearing)),
	                                Weather.GustSigmaMps, Weather.RainIntensity * 100.0f));

	// Designated track: kinematics from the same interpolated sample the
	// scope plots, so console and scope can never disagree.
	const TCHAR* StateNames[] = {TEXT("TENT"), TEXT("CONF"), TEXT("COAST")};
	bool bHaveTrack = false;
	TArray<FSeaEntityState> Entities;
	Net->SampleEntities(Entities);
	for (const FSeaEntityState& Entity : Entities)
	{
		if (Entity.Kind == ESeaEntityKind::Track && Entity.Id == SelectedTrackId)
		{
			bHaveTrack = true;
			CachedLines.Add(FString::Printf(
			    TEXT("TRK  #%02d %s  %s  SIG %.0fM"), Entity.Id,
			    StateNames[FMath::Clamp<int32>(Entity.State, 0, 2)], *RangeBearing(Entity.Position),
			    Entity.TrackSigmaM));
			break;
		}
	}
	if (!bHaveTrack)
	{
		CachedLines.Add(SelectedTrackId == 0 ? TEXT("TRK  -- NO DESIGNATION")
		                                     : FString::Printf(TEXT("TRK  #%02d -- LOST"),
		                                                       SelectedTrackId));
	}

	FSeaFireSolution Solution;
	if (bHaveTrack && Net->GetFireSolution(SelectedTrackId, Solution) && Solution.bValid)
	{
		CachedLines.Add(FString::Printf(TEXT("SOL  VALID  PIP %s  TOF %4.1fS"),
		                                *RangeBearing(Solution.Pip), Solution.TimeOfFlightS));
		CachedLines.Add(
		    FString::Printf(TEXT("     DISPERSION RADIUS %.0fM"), Solution.DispersionRadiusM));
	}
	else
	{
		CachedLines.Add(TEXT("SOL  ---- NO SOLUTION"));
		CachedLines.Add(TEXT(""));
	}

	FString Tally = FString::Printf(TEXT("TALLY LCH %d SPL %d BST %d KILL %d"), Launches,
	                                Splashes, Bursts, Kills);
	if (LastMissM >= 0.0f)
	{
		Tally += FString::Printf(TEXT(" MISS %.0fM"), LastMissM);
	}
	CachedLines.Add(Tally);
}
