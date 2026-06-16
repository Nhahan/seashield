#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateColorBrush.h"
#include "Rendering/DrawElements.h"

// Shared SeaShield HUD console styling — a cohesive tactical palette + a framed
// "combat console" panel (glassy fill + accent top rule + L-shaped corner
// brackets), so the gunsight / fire-control / PPI / game-HUD / AAR widgets read
// as one console instead of an ad-hoc wireframe overlay. Header-only inline (the
// SLeafWidget painters live in separate translation units). Symbology/logic is
// untouched — this is purely the chrome.
namespace SeaHud
{
// Tactical phosphor palette: teal-green primary readout, cyan frame accent,
// amber/green/red status. One source so the whole console stays consistent.
inline const FLinearColor Ink(0.62f, 1.00f, 0.78f, 0.96f);     // primary text
inline const FLinearColor Dim(0.40f, 0.72f, 0.58f, 0.85f);     // secondary / labels
inline const FLinearColor Accent(0.32f, 0.86f, 1.00f, 0.95f);  // cyan frame accent
inline const FLinearColor Amber(1.00f, 0.78f, 0.26f, 0.95f);
inline const FLinearColor Good(0.40f, 1.00f, 0.52f, 0.97f);
inline const FLinearColor Warn(1.00f, 0.60f, 0.20f, 0.95f);
inline const FLinearColor Bad(1.00f, 0.32f, 0.26f, 0.97f);
inline const FLinearColor PanelFill(0.012f, 0.035f, 0.028f, 0.66f);

// A framed console panel at (Pos, Size): glassy dark fill + a thin accent top
// rule + L-shaped corner brackets. Draws on [Layer .. Layer+1]; content goes on
// Layer+2 and up.
inline void ConsolePanel(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
                         const FVector2f& Pos, const FVector2f& Size,
                         const FLinearColor& AccentColor = Accent, float BracketLen = 13.0f)
{
	static const FSlateColorBrush Fill(FLinearColor::White);
	FSlateDrawElement::MakeBox(Out, Layer,
	                           Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Pos)), &Fill,
	                           ESlateDrawEffect::None, PanelFill);
	// Accent top rule (the panel's "title bar").
	FSlateDrawElement::MakeBox(Out, Layer + 1,
	                           Geometry.ToPaintGeometry(FVector2f(Size.X, 2.5f),
	                                                    FSlateLayoutTransform(Pos)),
	                           &Fill, ESlateDrawEffect::None, AccentColor);
	const FPaintGeometry Geo = Geometry.ToPaintGeometry();
	const float X0 = Pos.X, Y0 = Pos.Y, X1 = Pos.X + Size.X, Y1 = Pos.Y + Size.Y;
	auto L = [&](const FVector2D& A, const FVector2D& B)
	{
		FSlateDrawElement::MakeLines(Out, Layer + 1, Geo, {A, B}, ESlateDrawEffect::None, AccentColor,
		                             true, 1.7f);
	};
	L({X0, Y0}, {X0 + BracketLen, Y0});
	L({X0, Y0}, {X0, Y0 + BracketLen});
	L({X1, Y0}, {X1 - BracketLen, Y0});
	L({X1, Y0}, {X1, Y0 + BracketLen});
	L({X0, Y1}, {X0 + BracketLen, Y1});
	L({X0, Y1}, {X0, Y1 - BracketLen});
	L({X1, Y1}, {X1 - BracketLen, Y1});
	L({X1, Y1}, {X1, Y1 - BracketLen});
}
}  // namespace SeaHud
