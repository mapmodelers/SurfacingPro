// Copyright MapModelers. All Rights Reserved.

#include "SurfSession.h"
#include "SurfSettings.h"
#include "SurfLog.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace SurfSession
{
namespace
{
	FCriticalSection Lock;
	TUniquePtr<FArchive> File;
	FString Path;
	double StartSeconds = 0.0;
	int32 EventCount = 0;

	/**
	 * The last dozen things that happened, for a note to carry with it
	 * (S117a). Names and their detail, trimmed: enough to read the gesture
	 * back, not enough to matter to the file's size.
	 */
	TArray<FString> Recent;

	/** The burst in progress: its name, when it began, and what it swallowed. */
	FString BurstName;
	double BurstStart = 0.0;
	int32 BurstLines = 0;
	int32 BurstFrictions = 0;
	bool bInBurst = false;

	void Write(const TCHAR* Kind, const FString& Name, const FString& Detail)
	{
		FScopeLock Guard(&Lock);
		if (!File)
		{
			return;
		}

		const double Elapsed = FPlatformTime::Seconds() - StartSeconds;
		const FString Line = FString::Printf(TEXT("%.3f\t%s\t%s\t%s\n"), Elapsed, Kind, *Name, *Detail);

		// Frames and metrics are the file's background noise; a note wants
		// the things he did, and its own lines would only quote themselves.
		const FString KindText(Kind);
		if (KindText != TEXT("frame") && KindText != TEXT("frame_setup") && KindText != TEXT("metric")
			&& !KindText.StartsWith(TEXT("note")))
		{
			Recent.Add(Detail.IsEmpty()
				? FString::Printf(TEXT("%s %s"), Kind, *Name)
				: FString::Printf(TEXT("%s %s (%s)"), Kind, *Name, *Detail.Left(60)));
			while (Recent.Num() > 12)
			{
				Recent.RemoveAt(0);
			}
		}

		FTCHARToUTF8 Utf8(*Line);
		File->Serialize(const_cast<ANSICHAR*>(reinterpret_cast<const ANSICHAR*>(Utf8.Get())), Utf8.Length());

		// Flushed every line rather than on close: a session that ends in a
		// crash is exactly the one worth having a record of.
		File->Flush();
		++EventCount;
	}
}

void Begin()
{
	const USurfSettings* Settings = GetDefault<USurfSettings>();
	if (!Settings || !Settings->bRecordSessions)
	{
		return;
	}

	FScopeLock Guard(&Lock);
	if (File)
	{
		return;
	}

	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SurfacingPro"), TEXT("Sessions"));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);

	Path = FPaths::Combine(Directory, FString::Printf(TEXT("session-%s.tsv"), *FDateTime::Now().ToString(TEXT("%Y.%m.%d-%H.%M.%S"))));

	File.Reset(IFileManager::Get().CreateFileWriter(*Path, FILEWRITE_AllowRead));
	if (!File)
	{
		UE_LOG(LogSurfacingPro, Warning, TEXT("SurfacingPro could not open a session record at %s"), *Path);
		return;
	}

	StartSeconds = FPlatformTime::Seconds();
	EventCount = 0;

	const FString Header = TEXT("# elapsed\tkind\tname\tdetail\n");
	FTCHARToUTF8 Utf8(*Header);
	File->Serialize(const_cast<ANSICHAR*>(reinterpret_cast<const ANSICHAR*>(Utf8.Get())), Utf8.Length());
	File->Flush();

	UE_LOG(LogSurfacingPro, Log, TEXT("SurfacingPro is recording this session to %s"), *Path);
}

void End()
{
	FScopeLock Guard(&Lock);
	if (!File)
	{
		return;
	}

	File->Close();
	File.Reset();

	UE_LOG(LogSurfacingPro, Log, TEXT("SurfacingPro session record closed: %d events in %s"), EventCount, *Path);
}

bool IsRecording()
{
	FScopeLock Guard(&Lock);
	return File.IsValid();
}

namespace
{
	/** Whether a line is swallowed by the burst in progress (counted, not written). */
	bool Swallow(bool bFriction)
	{
		FScopeLock Guard(&Lock);
		if (!bInBurst)
		{
			return false;
		}
		++BurstLines;
		if (bFriction)
		{
			++BurstFrictions;
		}
		return true;
	}
}

void BeginBurst(const FString& Name)
{
	FScopeLock Guard(&Lock);
	if (bInBurst)
	{
		return;
	}
	bInBurst = true;
	BurstName = Name;
	BurstStart = FPlatformTime::Seconds();
	BurstLines = 0;
	BurstFrictions = 0;
}

void EndBurst()
{
	FString Name;
	FString Detail;
	{
		FScopeLock Guard(&Lock);
		if (!bInBurst)
		{
			return;
		}
		bInBurst = false;
		Name = BurstName;
		Detail = FString::Printf(TEXT("lines=%d frictions=%d seconds=%.1f"), BurstLines, BurstFrictions, FPlatformTime::Seconds() - BurstStart);
	}
	Write(TEXT("burst"), Name, Detail);
}

void Key(const FString& ActionName, const FString& Chord)      { EndBurst(); Write(TEXT("key"), ActionName, Chord); }
void Verb(const FString& Name, const FString& Detail)          { if (!Swallow(false)) { Write(TEXT("verb"), Name, Detail); } }
void Selection(const FString& Detail)                          { if (!Swallow(false)) { Write(TEXT("selection"), TEXT("changed"), Detail); } }
void Gesture(const FString& Name, const FString& Detail)       { EndBurst(); Write(TEXT("gesture"), Name, Detail); }
void Friction(const FString& Name, const FString& Detail)      { if (!Swallow(true)) { Write(TEXT("friction"), Name, Detail); } }
TDelegate<FString()> NoteStateProvider;

void Note(const FString& Text)
{
	EndBurst();
	Write(TEXT("note"), TEXT("user"), Text);

	// What he had just been doing, and where he was standing while he did it.
	// Written after the note so the note is still the line you find first.
	FString Before;
	{
		FScopeLock Guard(&Lock);
		for (const FString& Each : Recent)
		{
			Before += Before.IsEmpty() ? Each : (TEXT(" | ") + Each);
		}
	}
	if (!Before.IsEmpty())
	{
		Write(TEXT("note_before"), TEXT("user"), Before);
	}

	if (NoteStateProvider.IsBound())
	{
		const FString State = NoteStateProvider.Execute();
		if (!State.IsEmpty())
		{
			Write(TEXT("note_state"), TEXT("user"), State);
		}
	}
}

FString CurrentPath()
{
	FScopeLock Guard(&Lock);
	return Path;
}

} // namespace SurfSession
