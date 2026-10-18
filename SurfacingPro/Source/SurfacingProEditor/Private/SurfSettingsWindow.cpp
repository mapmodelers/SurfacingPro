// Copyright MapModelers. All Rights Reserved.

#include "SurfSettingsWindow.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "SurfSession.h"

namespace SurfSettingsWindow
{
	void Open()
	{
		// The editor's own preferences page (S247a, redundancy W3): the same
		// details view, built once by the editor, in the place a person
		// looks for a plugin's settings. Ours was a copy of it.
		if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			Settings->ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("SurfSettings"));
			SurfSession::Verb(TEXT("settings_window"), TEXT("preferences page"));
		}
	}
}
