// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Save sync in the app: its start with the app, and its page (Reader, Save sync in the main
// screen), made of the emulator's own interface parts so that every build has it.

#include <cstdint>
#include <string>

#include "UI/SimpleDialogScreen.h"

namespace UI {
class TextView;
}

namespace ReaderSaveSync {

/// With the app: reads the settings and brings the saves in line. A memory stick that is a
/// folder chosen with Android's file picker (a document tree) is reached through the
/// emulator's own file calls.
void Start();
/// Each frame: when the memory stick changed, save sync is ended and started again for it.
void Follow();
void Shutdown();

class Screen : public UISimpleBaseDialogScreen {
public:
	Screen();
	void update() override;

protected:
	std::string_view GetTitle() const override { return "Save sync"; }
	void CreateDialogViews(UI::ViewGroup *parent) override;
	const char *tag() const override { return "ReaderSaveSync"; }

private:
	void Apply();

	bool enabled_ = false;
	std::string addresses_;
	std::string code_;
	std::string device_;
	UI::TextView *status_ = nullptr;
	uint64_t seen_ = 0;
	size_t conflicts_ = 0;
};

}  // namespace ReaderSaveSync
