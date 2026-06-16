#include "SeaPpiWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SizeBox.h"
#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

#include "SeaHudStyle.h"

namespace {

// Polyline circle — the whole scope is vector line work, so one helper.
TArray<FVector2D> CirclePoints(const FVector2D& Center, float Radius, int32 Segments = 48)
{
	TArray<FVector2D> Points;
	Points.Reserve(Segments + 1);
	for (int32 i = 0; i <= Segments; ++i)
	{
		const float Angle = 2.0f * PI * static_cast<float>(i) / static_cast<float>(Segments);
		Points.Add(Center + FVector2D(FMath::Sin(Angle), -FMath::Cos(Angle)) * Radius);
	}
	return Points;
}

void DrawLines(FSlateWindowElementList& OutDrawElements, int32 LayerId, const FGeometry& Geometry,
               const TArray<FVector2D>& Points, const FLinearColor& Color, float Thickness = 1.0f)
{
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geometry.ToPaintGeometry(), Points,
	                             ESlateDrawEffect::None, Color, true, Thickness);
}

FVector2D ProjectToScope(const FVector2D& Size, float DisplayRangeM, double EastM, double NorthM)
{
	const FVector2D Center = Size * 0.5f;
	const float Radius = 0.5f * FMath::Min(Size.X, Size.Y) - 8.0f;
	const float Scale = Radius / FMath::Max(DisplayRangeM, 1.0f);
	// North-up: +north is up (-Y in widget space), +east is right.
	return Center + FVector2D(EastM * Scale, -NorthM * Scale);
}

}  // namespace

// The actual scope painter. A plain Slate leaf: paints every frame (volatile)
// straight from the owning USeaPpiWidget's cached data.
class SSeaPpiScope final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSeaPpiScope) {}
	SLATE_ARGUMENT(TWeakObjectPtr<USeaPpiWidget>, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;
		SetCanTick(false);
		ForceVolatile(true);  // The sweep animates; never cache the paint.
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(420.0, 420.0); }

	// Click-to-designate: the nearest track symbol within grab range becomes
	// the designated track (PIP + dispersion follow on scope and console).
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry,
	                                 const FPointerEvent& MouseEvent) override
	{
		USeaPpiWidget* Data = Owner.Get();
		if (Data == nullptr)
		{
			return FReply::Unhandled();
		}
		const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
		const FVector2D Size = MyGeometry.GetLocalSize();
		int32 Best = 0;
		float BestDistance = 24.0f;  // px grab radius
		for (const FSeaEntityState& Entity : Data->Entities())
		{
			if (Entity.Kind != ESeaEntityKind::Track)
			{
				continue;
			}
			const FVector2D At = ProjectToScope(Size, Data->DisplayRangeM,
			                                    Entity.Position.Y / 100.0, Entity.Position.X / 100.0);
			const float Distance = FVector2D::Distance(At, Local);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Entity.Id;
			}
		}
		if (Best != 0)
		{
			Data->SelectTrack(Best);
		}
		return FReply::Handled();
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	                      const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	                      int32 LayerId, const FWidgetStyle& InWidgetStyle,
	                      bool bParentEnabled) const override
	{
		const USeaPpiWidget* Data = Owner.Get();
		const FVector2D Size = AllottedGeometry.GetLocalSize();
		const FVector2D Center = Size * 0.5f;
		const float Radius = 0.5f * FMath::Min(Size.X, Size.Y) - 8.0f;
		if (Data == nullptr || Radius <= 0.0f)
		{
			return LayerId;
		}

		// Scope background: the phosphor line work needs a dark bezel to
		// read against the bright sea behind the HUD.
		static const FSlateColorBrush BackgroundBrush(FLinearColor::White);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		                           &BackgroundBrush, ESlateDrawEffect::None,
		                           FLinearColor(0.01f, 0.02f, 0.012f, 0.88f));
		++LayerId;

		// Bezel rim: a bright cyan ring + a faint inner ring, so the scope reads as
		// an instrument, not a bare ring stack. Cardinal + range labels round it out.
		DrawLines(OutDrawElements, LayerId, AllottedGeometry, CirclePoints(Center, Radius),
		          SeaHud::Accent, 2.2f);
		DrawLines(OutDrawElements, LayerId, AllottedGeometry, CirclePoints(Center, Radius - 4.0f),
		          SeaHud::Dim, 0.8f);
		{
			const FSlateFontInfo ScopeFont = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), 11);
			auto Label = [&](const FString& Str, const FVector2D& At, const FLinearColor& Col)
			{
				FSlateDrawElement::MakeText(
				    OutDrawElements, LayerId + 1,
				    AllottedGeometry.ToPaintGeometry(FVector2f(48, 16),
				                                     FSlateLayoutTransform(FVector2f(At.X, At.Y))),
				    Str, ScopeFont, ESlateDrawEffect::None, Col);
			};
			Label(TEXT("N"), Center + FVector2D(-4, -Radius + 3), SeaHud::Accent);
			Label(TEXT("E"), Center + FVector2D(Radius - 14, -8), SeaHud::Accent);
			Label(TEXT("S"), Center + FVector2D(-4, Radius - 18), SeaHud::Accent);
			Label(TEXT("W"), Center + FVector2D(-Radius + 5, -8), SeaHud::Accent);
			for (int32 i = 1; i <= 4; ++i)
			{
				const float Rkm = (Data->DisplayRangeM * i / 4.0f) / 1000.0f;
				Label(FString::Printf(TEXT("%.0f"), Rkm),
				      Center + FVector2D(4, -Radius * i / 4.0f - 1), SeaHud::Dim);
			}
		}

		// Range rings (quarters of the display range) + bearing ticks / 30°.
		for (int32 Ring = 1; Ring <= 4; ++Ring)
		{
			DrawLines(OutDrawElements, LayerId, AllottedGeometry,
			          CirclePoints(Center, Radius * Ring / 4.0f), Data->ScopeColor,
			          Ring == 4 ? 1.5f : 0.7f);
		}
		for (int32 Bearing = 0; Bearing < 360; Bearing += 30)
		{
			const float Angle = FMath::DegreesToRadians(static_cast<float>(Bearing));
			const FVector2D Direction(FMath::Sin(Angle), -FMath::Cos(Angle));
			DrawLines(OutDrawElements, LayerId, AllottedGeometry,
			          {Center + Direction * Radius * 0.95f, Center + Direction * Radius},
			          Data->ScopeColor);
		}

		// Engagement envelope: the effective unguided kill band (Rmin..Rmax).
		// Shots outside it are low-Pk (too far = the target out-maneuvers the long
		// flight + huge dispersion; too close = no time to correct), so the scope
		// shows the window the operator must wait for — matches the gun sight's
		// IN-ENVELOPE / HOLD cue (SeaGunsightWidget kEnvMin/MaxM).
		{
			const float MtoPx = Radius / FMath::Max(Data->DisplayRangeM, 1.0f);
			const FLinearColor EnvCol(0.35f, 1.0f, 0.5f, 0.5f);
			const float RminPx = 600.0f * MtoPx;
			const float RmaxPx = 4500.0f * MtoPx;
			if (RminPx > 2.0f && RminPx < Radius)
			{
				DrawLines(OutDrawElements, LayerId, AllottedGeometry,
				          CirclePoints(Center, RminPx), EnvCol, 1.3f);
			}
			if (RmaxPx < Radius)
			{
				DrawLines(OutDrawElements, LayerId, AllottedGeometry,
				          CirclePoints(Center, RmaxPx), EnvCol, 1.3f);
			}
		}

		// Rotating sweep with a short trailing fade.
		for (int32 Trail = 0; Trail < 6; ++Trail)
		{
			const float Angle = Data->SweepAngle() - 0.035f * static_cast<float>(Trail);
			const FVector2D Direction(FMath::Sin(Angle), -FMath::Cos(Angle));
			FLinearColor Fade = Data->TrackColor;
			Fade.A = 0.55f * (1.0f - static_cast<float>(Trail) / 6.0f);
			DrawLines(OutDrawElements, LayerId, AllottedGeometry,
			          {Center, Center + Direction * Radius}, Fade, Trail == 0 ? 1.6f : 1.0f);
		}

		// Own ship.
		DrawLines(OutDrawElements, LayerId + 1, AllottedGeometry, CirclePoints(Center, 5.0f, 12),
		          Data->TrackColor, 1.4f);

		// Track symbols: the estimate stream, with quality and lifecycle.
		USeaNetSubsystem* Net = nullptr;
		if (const UGameInstance* GameInstance = Data->GetGameInstance())
		{
			Net = GameInstance->GetSubsystem<USeaNetSubsystem>();
		}
		const float MetersToPx = Radius / FMath::Max(Data->DisplayRangeM, 1.0f);
		for (const FSeaEntityState& Entity : Data->Entities())
		{
			if (Entity.Kind != ESeaEntityKind::Track)
			{
				continue;
			}
			const double EastM = Entity.Position.Y / 100.0;
			const double NorthM = Entity.Position.X / 100.0;
			const FVector2D At = ProjectToScope(Size, Data->DisplayRangeM, EastM, NorthM);
			if (FVector2D::Distance(At, Center) > Radius)
			{
				continue;  // Beyond the displayed range.
			}

			FLinearColor Color = Data->TrackColor;
			if (Entity.State == 0)
			{
				Color.A = 0.5f;  // Tentative: dim.
			}
			else if (Entity.State == 2)
			{
				Color = FLinearColor(1.0f, 0.75f, 0.25f, 0.9f);  // Coasting: amber.
			}
			const int32 Designated = Net != nullptr ? Net->GetDesignatedTrack() : 0;
			const bool bSelected = Entity.Id == Designated && Designated != 0;
			const float Half = bSelected ? 10.0f : 7.0f;

			// NTDS-style hostile caret (^).
			DrawLines(OutDrawElements, LayerId + 2, AllottedGeometry,
			          {At + FVector2D(-Half, Half * 0.4f), At + FVector2D(0.0f, -Half),
			           At + FVector2D(Half, Half * 0.4f)},
			          Color, bSelected ? 2.2f : 1.4f);

			// Velocity leader (30 s of travel at current estimate).
			const FVector2D LeaderDir(Entity.Velocity.Y / 100.0, -Entity.Velocity.X / 100.0);
			DrawLines(OutDrawElements, LayerId + 2, AllottedGeometry,
			          {At, At + LeaderDir * (30.0f * MetersToPx)}, Color);

			// σ ring — the quantized track quality, drawn to scale.
			if (Entity.TrackSigmaM > 0.0f)
			{
				FLinearColor SigmaColor = Color;
				SigmaColor.A *= 0.45f;
				DrawLines(OutDrawElements, LayerId + 1, AllottedGeometry,
				          CirclePoints(At, Entity.TrackSigmaM * MetersToPx, 24), SigmaColor, 0.8f);
			}

			// Designated track: streamed PIP (X marker) + dispersion circle.
			FSeaFireSolution Solution;
			if (bSelected && Net != nullptr && Net->GetFireSolution(Entity.Id, Solution) &&
			    Solution.bValid)
			{
				const FVector2D Pip = ProjectToScope(Size, Data->DisplayRangeM,
				                                     Solution.Pip.Y / 100.0, Solution.Pip.X / 100.0);
				const FLinearColor PipColor(1.0f, 1.0f, 1.0f, 0.95f);
				DrawLines(OutDrawElements, LayerId + 3, AllottedGeometry,
				          {Pip + FVector2D(-6, -6), Pip + FVector2D(6, 6)}, PipColor, 1.6f);
				DrawLines(OutDrawElements, LayerId + 3, AllottedGeometry,
				          {Pip + FVector2D(-6, 6), Pip + FVector2D(6, -6)}, PipColor, 1.6f);
				// The pattern circle reflects the OPERATOR's ordered dispersion
				// (mrad x slant range), not the server's default-dispersion
				// preview in Solution.DispersionRadiusM — so the scope shows the
				// spread that will actually be fired as the order is adjusted.
				const float PipRangeM = Solution.Pip.Size() / 100.0f;
				const float OrderRadiusM = Net->GetOrderDispersionMrad() * 1.0e-3f * PipRangeM;
				DrawLines(OutDrawElements, LayerId + 3, AllottedGeometry,
				          CirclePoints(Pip, OrderRadiusM * MetersToPx, 32), PipColor, 0.9f);
			}
		}
		return LayerId + 4;
	}

private:
	TWeakObjectPtr<USeaPpiWidget> Owner;
};

TSharedRef<SWidget> USeaPpiScopeHost::RebuildWidget()
{
	TSharedRef<SSeaPpiScope> Scope = SNew(SSeaPpiScope).Owner(Owner);
	MyScope = Scope;
	return Scope;
}

void USeaPpiScopeHost::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyScope.Reset();
}

TSharedRef<SWidget> USeaPpiWidget::RebuildWidget()
{
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		// Layout lives INSIDE the tree (canvas slot, bottom-left): the widget
		// is added to the viewport fullscreen, since the point-anchored
		// viewport slot path proved unreliable on 5.7.
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		USeaPpiScopeHost* Host =
		    WidgetTree->ConstructWidget<USeaPpiScopeHost>(USeaPpiScopeHost::StaticClass());
		Host->Owner = this;
		if (UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Host))
		{
			Slot->SetAnchors(FAnchors(0.0f, 1.0f, 0.0f, 1.0f));
			Slot->SetAlignment(FVector2D(0.0f, 1.0f));
			Slot->SetPosition(FVector2D(24.0f, -24.0f));
			Slot->SetSize(FVector2D(420.0f, 420.0f));
		}
		ScopeHost = Host;
		WidgetTree->RootWidget = Root;
	}
	return Super::RebuildWidget();
}

void USeaPpiWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (SweepPeriodS > 0.0f)
	{
		SweepAngleRad = FMath::Fmod(SweepAngleRad + 2.0f * PI * InDeltaTime / SweepPeriodS,
		                            2.0f * PI);
	}
	CachedEntities.Reset();
	bool bShowScope = false;
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>())
		{
			Net->SampleEntities(CachedEntities);
			// Role gating: consoles get the radar picture; a pure Observer
			// watches the 3D world without HUD overlays.
			bShowScope = Net->IsWelcomed() && Net->GetRole() != ESeaRole::Observer;
		}
	}
	if (ScopeHost != nullptr)
	{
		ScopeHost->SetVisibility(bShowScope ? ESlateVisibility::Visible
		                                    : ESlateVisibility::Collapsed);
	}
}

void USeaPpiWidget::SelectTrack(int32 TrackId)
{
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>())
		{
			Net->DesignateTrack(TrackId);
		}
	}
}
