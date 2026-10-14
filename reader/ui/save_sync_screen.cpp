// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/File/Path.h"
#include "Common/System/OSD.h"
#include "Common/System/System.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Util/PathUtil.h"

#include "reader/save_sync/host.h"
#include "reader/ui/save_sync_screen.h"

namespace ReaderSaveSync {

namespace Host = SaveSync::Host;

void Start() {
	const Path saves = GetSysDirectory(DIRECTORY_SAVEDATA);
	const Path data = GetSysDirectory(DIRECTORY_SYSTEM) / "reader-savesync";
	if (saves.Type() != PathType::NATIVE || data.Type() != PathType::NATIVE) {
		return;
	}
	std::string name = System_GetProperty(SYSPROP_NAME);
	// "manufacturer:model" on Android.
	const size_t colon = name.find(':');
	if (colon != std::string::npos && colon + 1 < name.size()) {
		name = name.substr(colon + 1);
	}
	if (name.empty()) {
		name = "PPSSPP Reader";
	}
	Host::Start(saves.ToString(), data.ToString(), [](const std::string &text) {
		g_OSD.Show(OSDType::MESSAGE_INFO, text, 6.0f, "reader_savesync");
	}, name);
}

void Shutdown() {
	Host::Shutdown();
}

Screen::Screen() : UISimpleBaseDialogScreen(Path(), SimpleDialogFlags::ContentsCanScroll) {
	const Host::Settings settings = Host::Get();
	enabled_ = settings.enabled;
	addresses_ = settings.addresses;
	code_ = settings.code;
	device_ = settings.device;
}

void Screen::Apply() {
	Host::Settings settings;
	settings.enabled = enabled_;
	settings.addresses = addresses_;
	settings.code = code_;
	settings.device = device_;
	Host::Set(settings);
}

void Screen::CreateDialogViews(UI::ViewGroup *parent) {
	using namespace UI;

	if (!Host::Started()) {
		parent->Add(new TextView("Save sync needs the memory stick to be an ordinary folder. Here it is one that only Android's file picker reaches.", ALIGN_LEFT | FLAG_WRAP_TEXT, false, new LinearLayoutParams(Margins(12, 12))));
		return;
	}

	parent->Add(new ItemHeader("A computer of yours keeps the saves"));
	parent->Add(new CheckBox(&enabled_, "Sync saves", "Before a game starts, after each save and when it stops"))->OnClick.Add([this](UI::EventParams &) {
		Apply();
		if (enabled_) {
			Host::SyncAll();
		}
	});
	PopupTextInputChoice *address = parent->Add(new PopupTextInputChoice(GetRequesterToken(), &addresses_, "Address of the service", "", 200, screenManager()));
	address->OnChange.Add([this](UI::EventParams &) { Apply(); });
	PopupTextInputChoice *code = parent->Add(new PopupTextInputChoice(GetRequesterToken(), &code_, "Code", "", 64, screenManager()));
	code->OnChange.Add([this](UI::EventParams &) { Apply(); });
	PopupTextInputChoice *device = parent->Add(new PopupTextInputChoice(GetRequesterToken(), &device_, "This device's name", "", 40, screenManager()));
	device->OnChange.Add([this](UI::EventParams &) { Apply(); });
	parent->Add(new TextView("The address and the code are those of Eden Reader's save sync service (or of a computer on which Eden Reader keeps the saves): several addresses with commas between them, the port after a colon when it is not 8642.", ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(Margins(12, 6))));

	parent->Add(new ItemHeader("Now"));
	status_ = parent->Add(new TextView(Host::Status(), ALIGN_LEFT | FLAG_WRAP_TEXT, false, new LinearLayoutParams(Margins(12, 8))));
	parent->Add(new Choice("Check the connection"))->OnClick.Add([](UI::EventParams &) {
		Host::Check();
	});
	parent->Add(new Choice("Sync every game now"))->OnClick.Add([](UI::EventParams &) {
		if (Host::Get().enabled) {
			Host::SyncAll();
		}
	});

	const std::vector<Host::Conflict> conflicts = Host::Conflicts();
	conflicts_ = conflicts.size();
	for (const Host::Conflict &conflict : conflicts) {
		parent->Add(new ItemHeader((conflict.name.empty() ? conflict.disc : conflict.name) + ": two different saves"));
		parent->Add(new TextView(conflict.message, ALIGN_LEFT | FLAG_WRAP_TEXT, false, new LinearLayoutParams(Margins(12, 6))));
		const std::string title = conflict.title;
		parent->Add(new Choice("Use the other device's save (this one's is kept in the backups)"))->OnClick.Add([title](UI::EventParams &) {
			Host::Resolve(title, false);
		});
		parent->Add(new Choice("Use this device's save"))->OnClick.Add([title](UI::EventParams &) {
			Host::Resolve(title, true);
		});
	}
	seen_ = Host::Generation();
}

void Screen::update() {
	UISimpleBaseDialogScreen::update();
	const uint64_t generation = Host::Generation();
	if (generation == seen_) {
		return;
	}
	seen_ = generation;
	if (Host::Conflicts().size() != conflicts_) {
		RecreateViews();
		return;
	}
	if (status_) {
		status_->SetText(Host::Status());
	}
}

}  // namespace ReaderSaveSync
