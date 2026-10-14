// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Common/Data/Text/I18n.h"
#include "Common/File/AndroidStorage.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Common/Math/lin/vec3.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/StringUtils.h"
#include "Common/System/Display.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/Root.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "Core/KeyMap.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "Core/Util/RecentFiles.h"
#include "UI/Background.h"
#include "UI/BackgroundAudio.h"
#include "UI/GameInfoCache.h"
#include "UI/Theme.h"

#include "reader/core/emulator.h"
#include "reader/ui/xmb.h"

namespace ReaderXmb {

namespace {

// The user's choice of main screen is a file's presence, beside the emulator's settings:
// the emulator's own settings file is left as it is.
Path ChoiceFile() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "reader-menu-classic";
}

// The folders the games are looked for in, one to a line, chosen from this screen.
Path FoldersFile() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "reader-game-folders";
}

std::vector<std::string> GameFolders() {
	std::vector<std::string> folders;
	std::string text;
	if (File::ReadTextFileToString(FoldersFile(), &text)) {
		std::vector<std::string> lines;
		SplitString(text, '\n', lines);
		for (const std::string &line : lines) {
			const std::string folder(StripSpaces(line));
			if (!folder.empty()) {
				folders.push_back(folder);
			}
		}
	}
	return folders;
}

void AddGameFolder(const std::string &folder) {
	std::vector<std::string> folders = GameFolders();
	if (std::find(folders.begin(), folders.end(), folder) == folders.end()) {
		folders.push_back(folder);
	}
	std::string text;
	for (const std::string &line : folders) {
		text += line + "\n";
	}
	File::WriteStringToFile(true, text, FoldersFile());
}

/// Whether a text profile is there for the game of this ID.
bool Readable(const std::string &id) {
	if (id.empty()) {
		return false;
	}
	if (Reader::Emu::HasProfileCheck()) {
		return Reader::Emu::HasProfile(id);
	}
	static std::set<std::string> known;
	static bool listed = false;
	if (!listed) {
		listed = true;
		std::vector<File::FileInfo> files;
		g_VFS.GetFileListing("reader/profiles", &files, "lua");
		for (const auto &file : files) {
			known.insert(file.name.substr(0, file.name.size() - 4));
		}
	}
	return known.count(id) != 0;
}

/// The games: what lies in the folders chosen here, in the app's own games folder and in
/// the folder the emulator's own browser is at, then what was played lately from elsewhere.
std::vector<Path> Games() {
	std::vector<Path> games;
	// The same game reached by two paths (a link, a folder named twice) is listed once: by
	// its name, and for a file of some size by that size.
	std::set<std::string> seen;
	std::set<uint64_t> sizes;
	const auto add = [&](const Path &path) {
		if (!seen.insert(path.GetFilename()).second) {
			return;
		}
		File::FileInfo info;
		if (File::GetFileInfo(path, &info) && !info.isDirectory && info.size > 0x100000 && !sizes.insert(info.size).second) {
			return;
		}
		games.push_back(path);
	};
	std::vector<Path> folders;
	for (const std::string &folder : GameFolders()) {
		folders.emplace_back(folder);
	}
#if PPSSPP_PLATFORM(ANDROID)
	// Where a file manager or a computer can put games without any permission being asked.
	if (!g_extFilesDir.empty()) {
		folders.push_back(Path(g_extFilesDir) / "games");
	}
#endif
	if (!g_Config.currentDirectory.empty()) {
		folders.push_back(g_Config.currentDirectory);
	}
	for (const Path &folder : folders) {
		std::vector<File::FileInfo> files;
		File::GetFilesInDir(folder, &files, "iso:cso:chd:pbp:elf:prx:");
		std::sort(files.begin(), files.end(), [](const File::FileInfo &a, const File::FileInfo &b) { return a.name < b.name; });
		for (const auto &file : files) {
			if (!file.isDirectory) {
				add(file.fullName);
			}
		}
	}
	for (const std::string &recent : g_recentFiles.GetRecentFiles()) {
		add(Path(recent));
	}
	return games;
}

}  // namespace

std::vector<ReadableGame> ReadableGames() {
	std::vector<ReadableGame> out;
	if (!g_gameInfoCache) {
		return out;
	}
	for (const Path &path : Games()) {
		std::shared_ptr<GameInfo> info = g_gameInfoCache->GetInfo(nullptr, path, GameInfoFlags::PARAM_SFO);
		if (!info || !info->Ready(GameInfoFlags::PARAM_SFO) || !Readable(info->id)) {
			continue;
		}
		std::string title = info->GetTitle();
		if (title.empty()) {
			title = path.GetFilename();
		}
		out.push_back({info->id, title, path.ToString()});
	}
	return out;
}

namespace {

struct Entry {
	std::string title;
	std::string detail;
	ImageID image = ImageID::invalid();
	Path game;
	std::function<void()> action;
};

struct Category {
	std::string name;
	ImageID image;
	std::vector<Entry> entries;
	int selected = 0;
	float position = 0.0f; ///< Where the list is, in entries: eases towards `selected`
};

// What was chosen is kept while the screen is rebuilt (after a visit to the settings).
int g_category = -1;
std::vector<int> g_selected;

// The ground's colour goes with the month, as the console's does.
constexpr uint32_t MonthColors[12][2] = {
	{0xFF8A8F96, 0xFF34383E}, // January: silver
	{0xFF2FA3C9, 0xFF0D4A63}, // February: gold (ABGR)
	{0xFF4FB56A, 0xFF16502A}, // March: green
	{0xFFB678D9, 0xFF55285F}, // April: pink
	{0xFF52A63A, 0xFF1B4A12}, // May: leaf green
	{0xFFB0608A, 0xFF4E2343}, // June: violet
	{0xFFB9A534, 0xFF52470F}, // July: aqua
	{0xFFC27A2E, 0xFF5A300C}, // August: blue
	{0xFFA8507C, 0xFF4A1D3A}, // September: purple
	{0xFF2D8FD6, 0xFF0F3F66}, // October: amber
	{0xFF3A5F8C, 0xFF16263D}, // November: brown
	{0xFF3A3AC2, 0xFF12125A}, // December: red
};

float Ease(float from, float to, float dt, float rate) {
	const float next = to + (from - to) * std::exp(-dt * rate);
	return std::fabs(next - to) < 0.001f ? to : next;
}

uint32_t WithAlpha(uint32_t color, float alpha) {
	const uint32_t a = (uint32_t)(std::clamp(alpha, 0.0f, 1.0f) * ((color >> 24) & 0xFF));
	return (color & 0x00FFFFFF) | (a << 24);
}

/// The whole of the screen: the category bar, the chosen category's entries crossing it,
/// the ground with its waves, the date and time. It draws itself and takes the keys, the
/// touches and the mouse itself, so that everything can move as it does on the console.
class XmbView : public UI::View {
public:
	XmbView(std::vector<Category> categories, int start, UI::LayoutParams *params)
		: UI::View(params), categories_(std::move(categories)) {
		category_ = std::clamp(g_category >= 0 ? g_category : start, 0, (int)categories_.size() - 1);
		for (size_t i = 0; i < categories_.size(); ++i) {
			Category &c = categories_[i];
			if (i < g_selected.size()) {
				c.selected = std::clamp(g_selected[i], 0, std::max(0, (int)c.entries.size() - 1));
			}
			c.position = (float)c.selected;
		}
		position_ = (float)category_;
		last_ = time_now_d();
		rested_ = last_;
	}

	~XmbView() override {
		g_category = category_;
		g_selected.clear();
		for (const Category &c : categories_) {
			g_selected.push_back(c.selected);
		}
		g_BackgroundAudio.SetGame(Path());
	}

	bool CanBeFocused() const override { return true; }
	bool CanMoveFocus(UI::FocusMove) const override { return false; }
	std::string DescribeText() const override { return Chosen() ? Chosen()->title : ""; }

	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		w = dc.GetBounds().w;
		h = dc.GetBounds().h;
	}

	// ---- Input ----

	bool Key(const KeyInput &key) override {
		if (key.flags & KeyInputFlags::DOWN) {
			int direction = -1;
			switch (key.keyCode) {
			case NKCODE_DPAD_LEFT: direction = 0; break;
			case NKCODE_DPAD_RIGHT: direction = 1; break;
			case NKCODE_DPAD_UP: case NKCODE_EXT_MOUSEWHEEL_UP: direction = 2; break;
			case NKCODE_DPAD_DOWN: case NKCODE_EXT_MOUSEWHEEL_DOWN: direction = 3; break;
			default: break;
			}
			if (direction >= 0) {
				if (!(key.flags & KeyInputFlags::IS_REPEAT)) {
					Step(direction);
					const bool wheel = key.keyCode == NKCODE_EXT_MOUSEWHEEL_UP || key.keyCode == NKCODE_EXT_MOUSEWHEEL_DOWN;
					held_ = wheel ? -1 : direction;
					held_code_ = key.keyCode;
					next_repeat_ = time_now_d() + 0.38;
				}
				return true;
			}
			if (UI::IsAcceptKey(key) && !(key.flags & KeyInputFlags::IS_REPEAT)) {
				Activate();
				return true;
			}
		} else if (key.flags & KeyInputFlags::UP) {
			if (key.keyCode == held_code_) {
				held_ = -1;
			}
		}
		return false;
	}

	bool Touch(const TouchInput &touch) override {
		if ((touch.flags & TouchInputFlags::MOUSE) && !(touch.flags & TouchInputFlags::UP) && (touch.buttons & 1) == 0) {
			return true;
		}
		if (touch.flags & TouchInputFlags::DOWN) {
			if (touch_id_ < 0) {
				touch_id_ = touch.id;
				down_x_ = touch.x;
				down_y_ = touch.y;
				down_position_ = position_;
				down_list_ = Current().position;
				drag_ = Drag::None;
			}
		} else if (touch.id == touch_id_ && (touch.flags & TouchInputFlags::MOVE)) {
			const float dx = touch.x - down_x_;
			const float dy = touch.y - down_y_;
			const float slop = 12.0f * scale_;
			if (drag_ == Drag::None && (std::fabs(dx) > slop || std::fabs(dy) > slop)) {
				drag_ = std::fabs(dx) > std::fabs(dy) ? Drag::Bar : Drag::List;
			}
			if (drag_ == Drag::Bar) {
				position_ = std::clamp(down_position_ - dx / CategorySpacing(), -0.3f, (float)categories_.size() - 0.7f);
			} else if (drag_ == Drag::List) {
				Category &c = Current();
				c.position = std::clamp(down_list_ - dy / EntrySpacing(), -0.3f, (float)c.entries.size() - 0.7f);
			}
		} else if (touch.id == touch_id_ && (touch.flags & TouchInputFlags::UP)) {
			touch_id_ = -1;
			if (touch.flags & TouchInputFlags::CANCEL) {
				drag_ = Drag::None;
				return true;
			}
			if (drag_ == Drag::Bar) {
				Choose(std::clamp((int)std::lround(position_), 0, (int)categories_.size() - 1));
			} else if (drag_ == Drag::List) {
				Category &c = Current();
				Select(std::clamp((int)std::lround(c.position), 0, std::max(0, (int)c.entries.size() - 1)));
			} else {
				Tap(touch.x, touch.y);
			}
			drag_ = Drag::None;
		}
		return true;
	}

	void Update() override {
		View::Update();
		const double now = time_now_d();
		const float dt = std::clamp((float)(now - last_), 0.0f, 0.1f);
		last_ = now;
		if (held_ >= 0 && now >= next_repeat_) {
			Step(held_);
			next_repeat_ = now + 0.11;
		}
		if (drag_ != Drag::Bar) {
			position_ = Ease(position_, (float)category_, dt, 13.0f);
		}
		for (size_t i = 0; i < categories_.size(); ++i) {
			Category &c = categories_[i];
			if (drag_ == Drag::List && (int)i == category_) {
				continue;
			}
			c.position = Ease(c.position, (float)c.selected, dt, 15.0f);
		}
		list_alpha_ = Ease(list_alpha_, 1.0f, dt, 7.0f);

		// A game that has been the chosen one for a moment gets its own picture behind
		// everything and its own music, as on the console.
		const Entry *chosen = Chosen();
		const Path wanted = chosen && !chosen->game.empty() && now - rested_ > 0.6 ? chosen->game : Path();
		if (wanted != picture_) {
			if (!picture_.empty()) {
				fading_ = picture_;
				fading_alpha_ = picture_alpha_;
			}
			picture_ = wanted;
			picture_alpha_ = 0.0f;
			g_BackgroundAudio.SetGame(wanted);
		}
		picture_alpha_ = Ease(picture_alpha_, picture_.empty() ? 0.0f : 1.0f, dt, 4.0f);
		fading_alpha_ = Ease(fading_alpha_, 0.0f, dt, 5.0f);
	}

	// ---- Drawing ----

	void Draw(UIContext &dc) override {
		const Bounds &b = bounds_;
		scale_ = std::clamp(std::min(b.w / 960.0f, b.h / 544.0f), 0.75f, 1.6f);
		cross_x_ = b.x + std::max(150.0f * scale_, b.w * 0.2f);
		cross_y_ = b.y + b.h * 0.25f;
		const double now = time_now_d();

		DrawGround(dc, now);
		DrawClock(dc);

		// The bar of categories.
		for (int i = 0; i < (int)categories_.size(); ++i) {
			const float rel = (float)i - position_;
			const float x = cross_x_ + rel * CategorySpacing();
			if (x < b.x - 100.0f || x > b.x2() + 100.0f) {
				continue;
			}
			const float near = std::clamp(1.0f - std::fabs(rel), 0.0f, 1.0f);
			const float size = (1.25f + 0.75f * near) * scale_;
			dc.Draw()->DrawImage(categories_[i].image, x, cross_y_, size, WithAlpha(0xFFFFFFFF, 0.45f + 0.55f * near), ALIGN_CENTER);
			if (near > 0.05f) {
				dc.SetFontScale(0.72f * scale_, 0.72f * scale_);
				dc.DrawTextShadow(categories_[i].name, x, cross_y_ + 48.0f * scale_, WithAlpha(0xFFFFFFFF, near), ALIGN_CENTER);
			}
		}

		// The chosen category's entries: those before the chosen one above the bar, the
		// chosen one under it, the rest below.
		const Category &c = categories_[std::clamp((int)std::lround(position_), 0, (int)categories_.size() - 1)];
		const float shift = (position_ - std::round(position_)) * CategorySpacing();
		const float column_alpha = std::clamp(1.0f - std::fabs(position_ - std::round(position_)) * 2.4f, 0.0f, 1.0f) * list_alpha_;
		for (int i = 0; i < (int)c.entries.size(); ++i) {
			const float rel = (float)i - c.position;
			const float y = EntryY(rel);
			if (y < b.y - 80.0f || y > b.y2() + 80.0f) {
				continue;
			}
			DrawEntry(dc, c.entries[i], cross_x_ - shift, y, rel, column_alpha, now);
		}
		dc.SetFontScale(1.0f, 1.0f);
	}

private:
	enum class Drag { None, Bar, List };

	Category &Current() { return categories_[category_]; }
	const Category &Current() const { return categories_[category_]; }
	const Entry *Chosen() const {
		const Category &c = Current();
		return c.entries.empty() ? nullptr : &c.entries[std::clamp(c.selected, 0, (int)c.entries.size() - 1)];
	}

	float CategorySpacing() const { return 128.0f * scale_; }
	float EntrySpacing() const { return 62.0f * scale_; }

	/// Where an entry is drawn, by how far it is from the chosen one.
	float EntryY(float rel) const {
		const float chosen = cross_y_ + 122.0f * scale_;
		const float above = cross_y_ - 80.0f * scale_;
		if (rel >= 0.0f) {
			return chosen + rel * EntrySpacing();
		}
		if (rel >= -1.0f) {
			return chosen + rel * (chosen - above);
		}
		return above + (rel + 1.0f) * EntrySpacing() * 0.8f;
	}

	void Step(int direction) {
		switch (direction) {
		case 0: Choose(category_ - 1); break;
		case 1: Choose(category_ + 1); break;
		case 2: Select(Current().selected - 1); break;
		case 3: Select(Current().selected + 1); break;
		}
	}

	void Choose(int category) {
		if (category < 0 || category >= (int)categories_.size()) {
			return;
		}
		if (category != category_) {
			category_ = category;
			list_alpha_ = 0.0f;
			rested_ = time_now_d();
			UI::PlayUISound(UI::UISound::SELECT);
		}
	}

	void Select(int entry) {
		Category &c = Current();
		if (entry < 0 || entry >= (int)c.entries.size()) {
			return;
		}
		if (entry != c.selected) {
			c.selected = entry;
			rested_ = time_now_d();
			UI::PlayUISound(UI::UISound::SELECT);
		}
	}

	void Activate() {
		const Entry *entry = Chosen();
		if (entry && entry->action) {
			UI::PlayUISound(UI::UISound::CONFIRM);
			// Copied: what it does may rebuild this screen.
			const std::function<void()> action = entry->action;
			action();
		}
	}

	void Tap(float x, float y) {
		// A category?
		if (std::fabs(y - cross_y_) < 46.0f * scale_) {
			const int category = (int)std::lround(position_ + (x - cross_x_) / CategorySpacing());
			if (category >= 0 && category < (int)categories_.size()) {
				Choose(category);
			}
			return;
		}
		// An entry: the chosen one is started, another becomes the chosen one.
		const Category &c = Current();
		for (int i = 0; i < (int)c.entries.size(); ++i) {
			const float rel = (float)i - c.position;
			const float half = (std::fabs(rel) < 0.5f ? 34.0f : 27.0f) * scale_;
			if (std::fabs(y - EntryY(rel)) <= half && x > cross_x_ - 90.0f * scale_) {
				if (i == c.selected) {
					Activate();
				} else {
					Select(i);
				}
				return;
			}
		}
	}

	void DrawGround(UIContext &dc, double now) {
		const Bounds &b = bounds_;
		const std::time_t t = std::time(nullptr);
		std::tm local{};
#ifdef _WIN32
		localtime_s(&local, &t);
#else
		localtime_r(&t, &local);
#endif
		const uint32_t *colors = MonthColors[std::clamp(local.tm_mon, 0, 11)];
		dc.Flush();
		dc.BeginNoTex();
		dc.Draw()->RectVGradient(b.x, b.y, b.x2(), b.y2(), colors[0], colors[1]);
		dc.Flush();
		dc.Begin();

		// The chosen game's own picture over it, coming and going.
		if (!fading_.empty() && fading_alpha_ > 0.01f) {
			::DrawGameBackground(dc, fading_, Lin::Vec3(0.0f, 0.0f, 0.0f), fading_alpha_ * 0.8f);
		}
		if (!picture_.empty() && picture_alpha_ > 0.01f) {
			::DrawGameBackground(dc, picture_, Lin::Vec3(0.0f, 0.0f, 0.0f), picture_alpha_ * 0.8f);
		}

		// The ribbon of waves across the middle.
		dc.Flush();
		dc.BeginNoTex();
		const int steps = std::clamp((int)(b.w / 4.0f), 40, 400);
		const float step = b.w / (float)steps;
		const float strength = 1.0f - 0.6f * std::max(picture_alpha_, fading_alpha_);
		for (int band = 0; band < 3; ++band) {
			const float speed = 0.22f + 0.09f * band;
			const float base = 0.56f + 0.05f * band;
			const uint32_t light = WithAlpha(0xFFFFFFFF, (0.16f - 0.035f * band) * strength);
			for (int n = 0; n < steps; ++n) {
				const float x0 = b.x + n * step;
				const float x1 = x0 + step;
				const float i = (float)n * step * 1280.0f / b.w;
				const float wave = std::sin(i * (0.0042f + 0.0009f * band) + (float)now * speed * 2.6f) * 0.055f +
					std::sin(i * 0.0021f - (float)now * speed * 1.3f + band * 1.7f) * 0.035f + base;
				const float y = b.y + wave * b.h;
				const float thick = (26.0f + 16.0f * band) * scale_;
				dc.Draw()->RectVGradient(x0, y - 2.0f, x1, y, 0x00FFFFFF, light);
				dc.Draw()->RectVGradient(x0, y, x1, y + thick, light, 0x00FFFFFF);
			}
		}
		dc.Flush();
		dc.Begin();
	}

	void DrawClock(UIContext &dc) {
		const std::time_t t = std::time(nullptr);
		std::tm local{};
#ifdef _WIN32
		localtime_s(&local, &t);
#else
		localtime_r(&t, &local);
#endif
		char text[32];
		std::snprintf(text, sizeof(text), "%d/%d  %d:%02d", local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
		dc.SetFontScale(0.8f * scale_, 0.8f * scale_);
		dc.DrawTextShadow(text, bounds_.x2() - 28.0f * scale_, bounds_.y + 30.0f * scale_, 0xFFFFFFFF, ALIGN_VCENTER | ALIGN_RIGHT);
	}

	void DrawEntry(UIContext &dc, const Entry &entry, float x, float y, float rel, float alpha, double now) {
		const float near = std::clamp(1.0f - std::fabs(rel), 0.0f, 1.0f);
		const float fade = alpha * (0.5f + 0.5f * near);
		const uint32_t color = WithAlpha(0xFFFFFFFF, fade);
		std::string title = entry.title;
		std::string detail = entry.detail;
		const float icon_scale = (0.62f + 0.38f * near) * scale_;
		if (!entry.game.empty()) {
			std::shared_ptr<GameInfo> info = g_gameInfoCache->GetInfo(
				dc.GetDrawContext(), entry.game, GameInfoFlags::PARAM_SFO | GameInfoFlags::ICON);
			title = info->GetTitle();
			if (title.empty()) {
				title = entry.game.GetFilename();
			}
			detail = info->id;
			if (Readable(info->id)) {
				detail += detail.empty() ? "Text can be read" : "  \xc2\xb7  Text can be read";
			}
			if (info->icon.texture) {
				// A PSP game's icon is 144 by 80.
				const float h = 76.0f * icon_scale;
				const float w = h * info->icon.texture->Width() / std::max(1, info->icon.texture->Height());
				dc.Flush();
				dc.GetDrawContext()->BindTexture(0, info->icon.texture);
				dc.Draw()->DrawTexRect(x - w / 2.0f, y - h / 2.0f, x + w / 2.0f, y + h / 2.0f, 0, 0, 1, 1, color);
				dc.Flush();
				dc.RebindTexture();
			} else {
				dc.Draw()->DrawImage(ImageID("I_UMD"), x, y, 1.3f * icon_scale, color, ALIGN_CENTER);
			}
		} else if (entry.image.isValid()) {
			dc.Draw()->DrawImage(entry.image, x, y, 1.25f * icon_scale, color, ALIGN_CENTER);
		}
		const float text_x = x + 96.0f * scale_;
		const float size = (0.78f + 0.3f * near) * scale_;
		const bool with_detail = near > 0.5f && !detail.empty();
		dc.SetFontScale(size, size);
		if (near > 0.9f) {
			// The chosen entry's name breathes a little, as the console's does.
			const float glow = 0.75f + 0.25f * (float)std::sin(now * 3.2);
			dc.DrawTextShadow(title, text_x, y - (with_detail ? 11.0f * scale_ : 0.0f), WithAlpha(0xFFFFFFFF, alpha * glow), ALIGN_VCENTER | ALIGN_LEFT);
		} else {
			dc.DrawTextShadow(title, text_x, y - (with_detail ? 11.0f * scale_ * near : 0.0f), color, ALIGN_VCENTER | ALIGN_LEFT);
		}
		if (with_detail) {
			dc.SetFontScale(0.62f * scale_, 0.62f * scale_);
			dc.DrawTextShadow(detail, text_x, y + 17.0f * scale_, WithAlpha(0xFFFFFFFF, alpha * (near - 0.5f) * 1.6f), ALIGN_VCENTER | ALIGN_LEFT);
		}
	}

	std::vector<Category> categories_;
	int category_ = 0;
	float position_ = 0.0f;
	float list_alpha_ = 1.0f;
	float scale_ = 1.0f;
	float cross_x_ = 0.0f;
	float cross_y_ = 0.0f;
	double last_ = 0.0;
	double rested_ = 0.0;

	int held_ = -1;
	InputKeyCode held_code_ = NKCODE_UNKNOWN;
	double next_repeat_ = 0.0;

	int touch_id_ = -1;
	float down_x_ = 0.0f;
	float down_y_ = 0.0f;
	float down_position_ = 0.0f;
	float down_list_ = 0.0f;
	Drag drag_ = Drag::None;

	Path picture_;
	Path fading_;
	float picture_alpha_ = 0.0f;
	float fading_alpha_ = 0.0f;
};

}  // namespace

bool Enabled() {
	return !File::Exists(ChoiceFile());
}

void SetEnabled(bool enabled) {
	if (enabled) {
		File::Delete(ChoiceFile());
	} else {
		File::CreateEmptyFile(ChoiceFile());
	}
}

UI::ViewGroup *Build(const Host &host) {
	using namespace UI;

	auto mm = GetI18NCategory(I18NCat::MAINMENU);
	auto di = GetI18NCategory(I18NCat::DIALOG);

	// The emulator's other screens in the same manner, until that has held from one start to
	// the next: after that the choice of theme and background in the settings is the user's.
	const Path dressed = GetSysDirectory(DIRECTORY_SYSTEM) / "reader-menu-dressed";
	if (!File::Exists(dressed)) {
		if (g_Config.sThemeName == "PSP" && g_Config.iBackgroundAnimation == (int)BackgroundAnimation::WAVE) {
			File::CreateEmptyFile(dressed);
		} else {
			g_Config.iBackgroundAnimation = (int)BackgroundAnimation::WAVE;
			g_Config.sThemeName = "PSP";
			UpdateTheme();
		}
	}

	// Once, on a handheld with controls of its own: no controls drawn over the game. An
	// install made before the emulator was told of such a device has them switched on.
	const Path controls = GetSysDirectory(DIRECTORY_SYSTEM) / "reader-touch-controls-off";
	if (!File::Exists(controls) && KeyMap::HasBuiltinController(System_GetProperty(SYSPROP_NAME))) {
		g_Config.bShowTouchControls = false;
		File::CreateEmptyFile(controls);
	}

	std::vector<Category> categories;

	Category settings{std::string(mm->T("Game Settings", "Settings")), ImageID("I_GEAR")};
	settings.entries.push_back({"PPSSPP settings", "Graphics, controls, sound, network, system", ImageID("I_SLIDERS"), Path(), host.settings});
	settings.entries.push_back({"Game folders", "Choose a folder that has your games", ImageID("I_FOLDER_OPEN"), Path(), host.folder});
	settings.entries.push_back({"Classic menu", "PPSSPP's own main screen", ImageID("I_HAMBURGER"), Path(), host.classic});
	settings.entries.push_back({std::string(di->T("Exit")), "", ImageID("I_EXIT"), Path(), host.exit});

	Category reader{"Reader", ImageID("I_FILE")};
	const auto page = [](const char *what) {
		const std::string name = what;
		return [name] { Reader::Emu::OpenMenu(name); };
	};
	reader.entries.push_back({"Statistics", "What was read, by day and by game", ImageID("I_INFO"), Path(), page("stats")});
	reader.entries.push_back({"Game scripts", "The games whose text can be read, and scripts to install", ImageID("I_UMD"), Path(), page("games")});
	reader.entries.push_back({"Dictionaries", "Import and order dictionaries", ImageID("I_FILE_COPY"), Path(), page("dictionaries")});
	reader.entries.push_back({"Anki", "Where cards go and what is on them", ImageID("I_SEND"), Path(), page("anki")});
	reader.entries.push_back({"Save sync", "The same saves on every device of yours", ImageID("I_FOLDER_UPLOAD"), Path(), host.save_sync});
	reader.entries.push_back({"Reader settings", "Lookups, cursor, cards, statistics", ImageID("I_GEAR_SMALL"), Path(), page("settings")});

	Category game{std::string(mm->T("Games")), ImageID("I_UMD")};
	for (const Path &path : Games()) {
		const auto launch = host.launch;
		game.entries.push_back({"", "", ImageID::invalid(), path, [launch, path] { launch(path); }});
	}
	if (game.entries.empty()) {
		game.entries.push_back({"Choose where your games are", "A folder with ISO or CSO files", ImageID("I_FOLDER_OPEN"), Path(), host.folder});
	}
	game.entries.push_back({"Memory Stick", "Browse, install and the homebrew store: PPSSPP's own screen", ImageID("I_SDCARD"), Path(), host.classic});

	categories.push_back(std::move(settings));
	categories.push_back(std::move(reader));
	categories.push_back(std::move(game));

	AnchorLayout *root = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));
	XmbView *view = root->Add(new XmbView(std::move(categories), 2, new AnchorLayoutParams(FILL_PARENT, FILL_PARENT, 0.0f, 0.0f, 0.0f, 0.0f)));
	root->SetDefaultFocusView(view);
	return root;
}

uint32_t GroundColor() {
	const std::time_t t = std::time(nullptr);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &t);
#else
	localtime_r(&t, &local);
#endif
	// Between the top and the bottom of the main screen's ground, nearer the top.
	const uint32_t top = MonthColors[std::clamp(local.tm_mon, 0, 11)][0];
	const uint32_t bottom = MonthColors[std::clamp(local.tm_mon, 0, 11)][1];
	uint32_t out = 0xFF000000;
	for (int shift = 0; shift < 24; shift += 8) {
		const uint32_t a = (top >> shift) & 0xFF;
		const uint32_t b = (bottom >> shift) & 0xFF;
		out |= ((a * 2 + b) / 3) << shift;
	}
	return out;
}

void RememberFolder(const std::string &folder) {
	AddGameFolder(folder);
}

}  // namespace ReaderXmb
