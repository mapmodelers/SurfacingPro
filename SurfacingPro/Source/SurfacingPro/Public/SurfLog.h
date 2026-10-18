// Copyright MapModelers. All Rights Reserved.
//
// Machine-readable telemetry. These lines are not written for a human to
// read in a log window - they are written so the harness in ue/_run can
// grep them out of a 4000-line editor log and hand back a two-line answer.
//
// One line, one record, fixed key order, no prose:
//   SP_METRIC name=<id> value=<double> unit=<str> n=<int>
//   SP_DIAG   tag=<id> <free text>
//
// Metric names are dotted and stable, so a number can be compared against
// the same number from three sessions ago: preview.rebuild_ms,
// undo.transaction_kb, bake.section_count, and so on.

#pragma once

#include "CoreMinimal.h"
#include "SurfacingProModule.h"

/** One measurement. */
#define SURF_METRIC(Name, Value, Unit) \
	UE_LOG(LogSurfacingPro, Display, TEXT("SP_METRIC name=%s value=%.4f unit=%s n=1"), \
		TEXT(Name), static_cast<double>(Value), TEXT(Unit))

/** A measurement aggregated over N samples; Value is the total, not the mean. */
#define SURF_METRIC_N(Name, Value, Unit, Count) \
	UE_LOG(LogSurfacingPro, Display, TEXT("SP_METRIC name=%s value=%.4f unit=%s n=%d"), \
		TEXT(Name), static_cast<double>(Value), TEXT(Unit), static_cast<int32>(Count))

/** A tagged diagnostic the harness can pull out by tag. */
#define SURF_DIAG(Tag, Format, ...) \
	UE_LOG(LogSurfacingPro, Display, TEXT("SP_DIAG tag=%s ") Format, TEXT(Tag), ##__VA_ARGS__)

/**
 * Times its own scope and emits a metric on destruction. Count is carried so
 * the harness can report per-item cost without the call site doing the maths.
 */
struct FSurfScopedMetric
{
	FSurfScopedMetric(const TCHAR* InName, int32 InCount = 1)
		: Name(InName)
		, Count(InCount)
		, StartSeconds(FPlatformTime::Seconds())
	{
	}

	~FSurfScopedMetric()
	{
		const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		UE_LOG(LogSurfacingPro, Display, TEXT("SP_METRIC name=%s value=%.4f unit=ms n=%d"),
			Name, ElapsedMs, Count);
	}

	/** Milliseconds elapsed so far, for a call site that also wants to assert on it. */
	double ElapsedMs() const { return (FPlatformTime::Seconds() - StartSeconds) * 1000.0; }

private:
	const TCHAR* Name;
	int32 Count;
	double StartSeconds;
};

#define SURF_SCOPED_METRIC(Name) FSurfScopedMetric PREPROCESSOR_JOIN(SurfScopedMetric_, __LINE__)(TEXT(Name))
#define SURF_SCOPED_METRIC_N(Name, Count) FSurfScopedMetric PREPROCESSOR_JOIN(SurfScopedMetric_, __LINE__)(TEXT(Name), (Count))
