// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <thread>

#include "Common/File/AndroidStorage.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/System/OSD.h"
#include "Common/System/System.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "Core/Util/PathUtil.h"

#include "reader/save_sync/archive.h"
#include "reader/save_sync/host.h"
#include "reader/ui/save_sync_screen.h"

namespace ReaderSaveSync {

namespace Host = SaveSync::Host;

namespace {

/// The memory stick save sync was started for (or found not set).
Path g_memstick;
bool g_followed = false;
std::atomic<bool> g_restarting{false};
/// Why it was not started; empty when it was.
std::string g_why_not;

/// The store's calls come from save sync's own threads too: on Android each is to be known to
/// Java for the file picker's folders, and let go when it ends.
struct JniThread {
	bool attached = false;
	~JniThread() {
		if (attached) {
			DetachThreadFromJNI();
		}
	}
};
thread_local JniThread t_jni;

void Jni() {
#if PPSSPP_PLATFORM(ANDROID)
	if (!t_jni.attached) {
		AttachThreadToJNI();
		t_jni.attached = true;
	}
#endif
}

Path Below(const Path &base, const std::string &path) {
	Path at = base;
	size_t start = 0;
	while (start < path.size()) {
		size_t slash = path.find('/', start);
		if (slash == std::string::npos) {
			slash = path.size();
		}
		if (slash > start) {
			at = at / std::string_view(path).substr(start, slash - start);
		}
		start = slash + 1;
	}
	return at;
}

/// SAVEDATA of a memory stick that is a document tree, through the emulator's own file calls.
Host::Store StoreFor(const Path &savedata) {
	Host::Store store;
	store.list = [savedata](const std::string &path, std::vector<Host::Store::Entry> &entries) {
		Jni();
		std::vector<File::FileInfo> files;
		if (!File::GetFilesInDir(Below(savedata, path), &files, nullptr, File::GETFILES_GETHIDDEN)) {
			return false;
		}
		entries.clear();
		for (const File::FileInfo &file : files) {
			Host::Store::Entry entry;
			entry.name = file.name;
			entry.folder = file.isDirectory;
			entry.modified_ms = static_cast<int64_t>(file.mtime) * 1000 + file.mtimeUs / 1000;
			entries.push_back(entry);
		}
		return true;
	};
	store.read = [savedata](const std::string &path, std::string &content) {
		Jni();
		return File::ReadBinaryFileToString(Below(savedata, path), &content);
	};
	store.write = [savedata](const std::string &path, std::string_view content) {
		Jni();
		return File::WriteDataToFile(false, content.data(), content.size(), Below(savedata, path));
	};
	store.make_folder = [savedata](const std::string &path) {
		Jni();
		const Path at = Below(savedata, path);
		return File::CreateDir(at) || File::IsDirectory(at);
	};
	store.remove = [savedata](const std::string &path) {
		Jni();
		const Path at = Below(savedata, path);
		if (!File::Exists(at)) {
			return true;
		}
		if (File::IsDirectory(at)) {
			File::DeleteDirRecursively(at);
		} else {
			File::Delete(at, true);
		}
		return !File::Exists(at);
	};
	store.rename = [savedata](const std::string &path, const std::string &name) {
		Jni();
		return File::Rename(Below(savedata, path), savedata / name);
	};
	return store;
}

/// Folders of save sync's own on this system's storage, where Android's file picker is not
/// needed.
Path OwnFolder() {
#if PPSSPP_PLATFORM(ANDROID)
	if (!g_extFilesDir.empty()) {
		return Path(g_extFilesDir);
	}
#endif
	return g_Config.internalDataDirectory;
}

/// What a start needs, gathered on the interface's thread (the settings are not for others).
struct Plan {
	bool ready = false;
	std::string why_not;
	Path savedata;
	Path data;
	bool store = false;
	std::vector<std::string> settings_from;
	std::string name;
};

Plan PlanFor(const Path &previous_data) {
	Plan plan;
	if (g_Config.memStickDirectory.empty()) {
		plan.why_not = "There is no memory stick yet.";
		return plan;
	}
	plan.savedata = GetSysDirectory(DIRECTORY_SAVEDATA);
	switch (plan.savedata.Type()) {
	case PathType::NATIVE:
		plan.data = GetSysDirectory(DIRECTORY_SYSTEM) / "reader-savesync";
		break;
	case PathType::CONTENT_URI: {
		// A folder chosen with Android's file picker: save sync's own folder is the app's, one
		// for each such memory stick.
		const Path own = OwnFolder();
		if (own.Type() != PathType::NATIVE) {
			plan.why_not = "Save sync found no folder of the app's own for its notes.";
			return plan;
		}
		plan.data = own / "reader-savesync" / SaveSync::Sha256Hex(plan.savedata.ToString()).substr(0, 16);
		plan.store = true;
		break;
	}
	default:
		plan.why_not = "Save sync cannot reach this memory stick.";
		return plan;
	}
	// The settings come along from the memory stick before, or from the app's own folders.
	if (!previous_data.empty() && previous_data.Type() == PathType::NATIVE && previous_data != plan.data) {
		plan.settings_from.push_back(previous_data.ToString());
	}
	for (const Path &stick : {OwnFolder(), g_Config.internalDataDirectory}) {
		if (stick.Type() == PathType::NATIVE) {
			const Path other = stick / "PSP" / "SYSTEM" / "reader-savesync";
			if (other != plan.data) {
				plan.settings_from.push_back(other.ToString());
			}
		}
	}
	std::string name = System_GetProperty(SYSPROP_NAME);
	// "manufacturer:model" on Android.
	const size_t colon = name.find(':');
	if (colon != std::string::npos && colon + 1 < name.size()) {
		name = name.substr(colon + 1);
	}
	plan.name = name.empty() ? "PPSSPP Reader" : name;
	plan.ready = true;
	return plan;
}

void Carry(const Plan &plan) {
	if (!plan.ready) {
		Host::Shutdown();
		return;
	}
	Host::Start(plan.savedata.ToString(), plan.data.ToString(), [](const std::string &text) {
		g_OSD.Show(OSDType::MESSAGE_INFO, text, 6.0f, "reader_savesync");
	}, plan.name, plan.store ? StoreFor(plan.savedata) : Host::Store{}, plan.settings_from);
}

}  // namespace

void Start() {
	g_memstick = g_Config.memStickDirectory;
	g_followed = true;
	const Plan plan = PlanFor(Path(Host::DataFolder()));
	g_why_not = plan.why_not;
	Carry(plan);
}

void Follow() {
	if (!g_followed || g_restarting || g_Config.memStickDirectory == g_memstick) {
		return;
	}
	// The memory stick changed: what ran is ended (it waits for what is still being sent) and
	// save sync starts again on the new one, beside the interface.
	g_memstick = g_Config.memStickDirectory;
	const Plan plan = PlanFor(Path(Host::DataFolder()));
	g_why_not = plan.why_not;
	g_restarting = true;
	std::thread([plan] {
		SetCurrentThreadName("ReaderSaveSync");
		Carry(plan);
		g_restarting = false;
	}).detach();
}

void Shutdown() {
	g_followed = false;
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
		const std::string why = g_restarting ? std::string("Save sync is starting again for the new memory stick.") : g_why_not.empty() ? std::string("Save sync did not start.") : g_why_not;
		parent->Add(new TextView(why, ALIGN_LEFT | FLAG_WRAP_TEXT, false, new LinearLayoutParams(Margins(12, 12))));
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
	if (Host::Staged()) {
		parent->Add(new TextView("The memory stick is a folder chosen with Android's file picker. Save sync copies a game's save folders from it into a folder of the app's own before it looks at them, and writes a save it fetched back to the memory stick. Saves it replaced are kept in " + Host::DataFolder() + "/backups.", ALIGN_LEFT | FLAG_WRAP_TEXT, true, new LinearLayoutParams(Margins(12, 6))));
	}

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
