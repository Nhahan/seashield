#include "SeaPpiWidget.h"

#include "Rendering/DrawElements.h"

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

}  // namespace

void USeaPpiWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (SweepPeriodS > 0.0f)
	{
		SweepAngleRad = FMath::Fmod(SweepAngleRad + 2.0f * PI * InDeltaTime / SweepPeriodS,
		                            2.0f * PI);
	}
	CachedEntities.Reset();
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		if (USeaNetSubsystem* Net = GameInstance->GetSubsystem<USeaNetSubsystem>())
		{
			Net->SampleEntities(CachedEntities);
		}
	}
}

FVector2D USeaPpiWidget::ToScope(const FGeometry& Geometry, double EastM, double NorthM) const
{
	const FVector2D Size = Geometry.GetLocalSize();
	const FVector2D Center = Size * 0.5f;
	const float Radius = 0.5f * FMath::Min(Size.X, Size.Y) - 8.0f;
	const float Scale = Radius / FMath::Max(DisplayRangeM, 1.0f);
	// North-up: +north is up (-Y in widget space), +east is right.
	return Center + FVector2D(EastM * Scale, -NorthM * Scale);
}

int32 USeaPpiWidget::TrackAtPosition(const FVector2D& LocalPoint, float GrabRadiusPx) const
{
	int32 Best = 0;
	float BestDistance = GrabRadiusPx;
	const FGeometry& Geometry = GetCachedGeometry();
	for (const FSeaEntityState& Entity : CachedEntities)
	{
		if (Entity.Kind != ESeaEntityKind::Track)
		{
			continue;
		}
		// Subsystem positions are UE cm; back to ENU meters for projection.
		const double EastM = Entity.Position.Y / 100.0;
		const double NorthM = Entity.Position.X / 100.0;
		const float Distance = FVector2D::Distance(ToScope(Geometry, EastM, NorthM), LocalPoint);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Entity.Id;
		}
	}
	return Best;
}

int32 USeaPpiWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
                                 const FSlateRect& MyCullingRect,
                                 FSlateWindowElementList& OutDrawElements, int32 LayerId,
                                 const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	LayerId = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId,
	                             InWidgetStyle, bParentEnabled);
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	const FVector2D Center = Size * 0.5f;
	const float Radius = 0.5f * FMath::Min(Size.X, Size.Y) - 8.0f;
	if (Radius <= 0.0f)
	{
		return LayerId;
	}

	// Range rings (quarters of the display range) + bearing ticks every 30°.
	for (int32 Ring = 1; Ring <= 4; ++Ring)
	{
		DrawLines(OutDrawElements, LayerId, AllottedGeometry,
		          CirclePoints(Center, Radius * Ring / 4.0f), ScopeColor, Ring == 4 ? 1.5f : 0.7f);
	}
	for (int32 Bearing = 0; Bearing < 360; Bearing += 30)
	{
		const float Angle = FMath::DegreesToRadians(static_cast<float>(Bearing));
		const FVector2D Direction(FMath::Sin(Angle), -FMath::Cos(Angle));
		DrawLines(OutDrawElements, LayerId, AllottedGeometry,
		          {Center + Direction * Radius * 0.95f, Center + Direction * Radius}, ScopeColor);
	}

	// Rotating sweep with a short trailing fade.
	for (int32 Trail = 0; Trail < 6; ++Trail)
	{
		const float Angle = SweepAngleRad - 0.035f * static_cast<float>(Trail);
		const FVector2D Direction(FMath::Sin(Angle), -FMath::Cos(Angle));
		FLinearColor Fade = TrackColor;
		Fade.A = 0.55f * (1.0f - static_cast<float>(Trail) / 6.0f);
		DrawLines(OutDrawElements, LayerId, AllottedGeometry, {Center, Center + Direction * Radius},
		          Fade, Trail == 0 ? 1.6f : 1.0f);
	}

	// Own ship.
	DrawLines(OutDrawElements, LayerId + 1, AllottedGeometry, CirclePoints(Center, 5.0f, 12),
	          TrackColor, 1.4f);

	// Track symbols: the estimate stream, with quality and lifecycle visible.
	USeaNetSubsystem* Net = nullptr;
	if (const UGameInstance* GameInstance = GetGameInstance())
	{
		Net = GameInstance->GetSubsystem<USeaNetSubsystem>();
	}
	const float MetersToPx = Radius / FMath::Max(DisplayRangeM, 1.0f);
	for (const FSeaEntityState& Entity : CachedEntities)
	{
		if (Entity.Kind != ESeaEntityKind::Track)
		{
			continue;
		}
		const double EastM = Entity.Position.Y / 100.0;
		const double NorthM = Entity.Position.X / 100.0;
		const FVector2D At = ToScope(AllottedGeometry, EastM, NorthM);
		if (FVector2D::Distance(At, Center) > Radius)
		{
			continue;  // Beyond the displayed range.
		}

		FLinearColor Color = TrackColor;
		if (Entity.State == 0)
		{
			Color.A = 0.5f;  // Tentative: dim.
		}
		else if (Entity.State == 2)
		{
			Color = FLinearColor(1.0f, 0.75f, 0.25f, 0.9f);  // Coasting: amber.
		}
		const bool bSelected = Entity.Id == SelectedTrackId && SelectedTrackId != 0;
		const float Half = bSelected ? 10.0f : 7.0f;

		// NTDS-style hostile caret (^) over a diamond base.
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

		// Designated track: streamed PIP (X marker) + 1σ dispersion circle.
		FSeaFireSolution Solution;
		if (bSelected && Net != nullptr && Net->GetFireSolution(Entity.Id, Solution) &&
		    Solution.bValid)
		{
			const FVector2D Pip =
			    ToScope(AllottedGeometry, Solution.Pip.Y / 100.0, Solution.Pip.X / 100.0);
			const FLinearColor PipColor(1.0f, 1.0f, 1.0f, 0.95f);
			DrawLines(OutDrawElements, LayerId + 3, AllottedGeometry,
			          {Pip + FVector2D(-6, -6), Pip + FVector2D(6, 6)}, PipColor, 1.6f);
			DrawLines(OutDrawElements, LayerId + 3, AllottedGeometry,
			          {Pip + FVector2D(-6, 6), Pip + FVector2D(6, -6)}, PipColor, 1.6f);
			DrawLines(OutDrawElements, LayerId + 3, AllottedGeometry,
			          CirclePoints(Pip, Solution.DispersionRadiusM * MetersToPx, 32), PipColor,
			          0.9f);
		}
	}
	return LayerId + 4;
}
