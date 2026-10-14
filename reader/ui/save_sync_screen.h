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

/// With the app: reads the settings and brings the saves in line. Nothing happens where the
/// memory stick is not a plain folder (Android's document trees).
void Start();
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
