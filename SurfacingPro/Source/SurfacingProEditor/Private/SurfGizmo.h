// Copyright MapModelers. All Rights Reserved.
//
// The handles of our own gizmo, as the editor sees them, and the ring maths.
//
// The editor's transform widget is kept alive but not drawn while ours is on:
// the screen-space axis directions it works out every frame, the tracking
// that turns a mouse drag into a move along an axis or a scale - all of it
// runs unchanged, because our handles' hit proxies are its own kind.
// HWidgetAxis is what the editor looks for when the cursor moves and when a
// drag begins; a subclass of it carries, in addition, the widget mode the
// handle belongs to, so hovering a ring puts the editor in rotate and hovering
// an arrow puts it back in move. Three gizmos become one.
//
// Rotation is the exception: the editor's ring maths is only worked out when
// its rings are drawn, so a ring of ours turns by the angle the cursor sweeps
// about the pivot on screen - which is also what a ring should do.

#pragma once

#include "CoreMinimal.h"
#include "UnrealWidgetFwd.h"
#include "UnrealWidget.h"

struct HSurfGizmoAxis : public HWidgetAxis
{
	DECLARE_HIT_PROXY();

	/** Move, rotate or scale: what the handle does, set on the viewport when the cursor reaches it. */
	UE::Widget::EWidgetMode Mode;

	HSurfGizmoAxis(EAxisList::Type InAxis, UE::Widget::EWidgetMode InMode)
		: HWidgetAxis(InAxis, /*InbDisabled*/ false)
		, Mode(InMode)
	{
	}

	// The editor's own answers, given again here: HWidgetAxis does not
	// export these two, so a subclass has to carry them to link.
	virtual EMouseCursor::Type GetMouseCursor() override { return EMouseCursor::CardinalCross; }
	virtual bool AlwaysAllowsTranslucentPrimitives() const override { return true; }
};

namespace SurfGizmo
{
	/**
	 * How much of the rotate rings' radius the ball takes (S139a).
	 *
	 * It was nearly all of it, which put a filled handle over the middle of
	 * the selection - "i cant click to select side and mode". Smaller, so
	 * the rings are reached without crossing it and there is room around
	 * the pivot; a press on it that turns nothing falls through to the pick
	 * in any case.
	 *
	 * And back up to the rings (S157f, A23): "could the gizmo rotate ring be
	 * on the same radius as the transparent sphere so it's easier to use?"
	 * The ball is only up while rotate is the mode, when the pick under it
	 * is not what the hand is after; a hair inside the rings so they stay
	 * their own grab.
	 */
	constexpr double BallShare = 0.94;

	/**
	 * The angle the cursor swept about a pivot on screen, from one offset to
	 * the next, in radians. Clockwise on the screen is positive: pixel rows
	 * grow downwards, so the usual atan2 reads that way round.
	 */
	inline double SweptRadians(const FVector2D& FromPivot, const FVector2D& ToPivot)
	{
		if (FromPivot.IsNearlyZero() || ToPivot.IsNearlyZero())
		{
			return 0.0;
		}
		return FMath::UnwindRadians(FMath::Atan2(ToPivot.Y, ToPivot.X) - FMath::Atan2(FromPivot.Y, FromPivot.X));
	}

	/**
	 * Where a point on the screen lands on a ball drawn round the pivot
	 * (S130a): a unit vector, in world space, from the ball's middle out to
	 * the point under the cursor.
	 *
	 * "Instead of rings for x y z rotation, overlay some sphere, in
	 * transparency, and we re rotating that sphere, 360 all axis possible!"
	 *
	 * Shoemake's virtual trackball, which is what every modelling tool that
	 * has one uses. Inside the silhouette the ball is a sphere and the
	 * cursor lands on the front of it; **outside it the ball has run out**,
	 * so the point is taken on the rim instead - which turns a drag past the
	 * edge into a spin about the axis you are looking down, and that is what
	 * a hand expects when it pushes past the side of a ball rather than an
	 * answer that suddenly stops.
	 *
	 * Offset is in pixels from the pivot, screen-style: Y grows downwards.
	 */
	inline FVector BallPoint(const FVector2D& Offset, double RadiusPixels,
		const FVector& Right, const FVector& Up, const FVector& Forward)
	{
		if (RadiusPixels <= KINDA_SMALL_NUMBER)
		{
			return FVector::ZeroVector;
		}

		const double Across = Offset.X / RadiusPixels;
		const double Down = -Offset.Y / RadiusPixels;
		const double Flat = Across * Across + Down * Down;

		if (Flat >= 1.0)
		{
			const double Shrink = 1.0 / FMath::Sqrt(Flat);
			return (Right * (Across * Shrink) + Up * (Down * Shrink)).GetSafeNormal();
		}

		// Toward the camera is the way the front of the ball faces, and the
		// camera looks along Forward - so the front is -Forward.
		return (Right * Across + Up * Down - Forward * FMath::Sqrt(1.0 - Flat)).GetSafeNormal();
	}

	/**
	 * The turn that carries one point on the ball to another - the whole of
	 * the trackball, in one line of geometry. Nothing when they are the same
	 * point, or opposite ones, where no axis is decided.
	 */
	inline FRotator BallRotation(const FVector& From, const FVector& To)
	{
		const FVector Axis = FVector::CrossProduct(From, To);
		if (Axis.IsNearlyZero(1.e-8))
		{
			return FRotator::ZeroRotator;
		}

		const double Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(From, To), -1.0, 1.0));
		if (Angle < 1.e-5)
		{
			return FRotator::ZeroRotator;
		}
		return FQuat(Axis.GetSafeNormal(), Angle).Rotator();
	}

	/**
	 * A turn of so many degrees about a ring's axis for a clockwise sweep on
	 * screen. About the axis as it points at the camera: a positive turn
	 * about an axis pointing at the viewer is clockwise to the viewer, so the
	 * ring follows the hand whichever side it is seen from.
	 */
	inline FRotator RingRotation(const FVector& Axis, const FVector& ToCamera, double Degrees)
	{
		FVector Facing = Axis.GetSafeNormal();
		if (Facing.IsNearlyZero())
		{
			return FRotator::ZeroRotator;
		}
		if (FVector::DotProduct(Facing, ToCamera) < 0.0)
		{
			Facing = -Facing;
		}
		return FQuat(Facing, FMath::DegreesToRadians(Degrees)).Rotator();
	}
}
