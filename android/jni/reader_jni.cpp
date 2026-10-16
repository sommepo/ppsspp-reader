// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-2.0-or-later

// The reader's bridge to its Kotlin layer (org.ppsspp.ppsspp.reader.ReaderNative).

#include <condition_variable>
#include <deque>
#include <functional>
#include <jni.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Common/Log.h"
#include "Common/System/Display.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/System.h"
#include "GPU/Common/PresentationCommon.h"

#include "reader/core/audio_tap.h"
#include "reader/core/emulator.h"
#include "reader/core/engine.h"
#include "reader/core/input_filter.h"
#include "reader/save_sync/host.h"
#include "reader/core/crt_jni.h"

extern JavaVM *gJvm;

namespace ReaderJni {
namespace {

jclass s_bridge_class;
jmethodID s_on_text_event;
jmethodID s_on_layout;
jmethodID s_on_log;
jmethodID s_on_profile_status;
jmethodID s_find_profile;
jmethodID s_load_profile_data;
jmethodID s_on_game_started;
jmethodID s_on_game_stopped;
jmethodID s_on_menu;

jbyteArray ToByteArray(JNIEnv *env, const std::string &text) {
	const jbyteArray array = env->NewByteArray(static_cast<jsize>(text.size()));
	if (array) {
		env->SetByteArrayRegion(array, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte *>(text.data()));
	}
	return array;
}

std::optional<std::string> FromByteArray(JNIEnv *env, jbyteArray array) {
	if (!array) {
		return std::nullopt;
	}
	const jsize size = env->GetArrayLength(array);
	std::string out(static_cast<std::size_t>(size), '\0');
	env->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte *>(out.data()));
	return out;
}

bool ClearException(JNIEnv *env, const char *what) {
	if (!env->ExceptionCheck()) {
		return false;
	}
	env->ExceptionDescribe();
	env->ExceptionClear();
	ERROR_LOG(Log::System, "[reader] Kotlin callback %s threw", what);
	return true;
}

/**
 * Everything the native side says to Kotlin is said on one thread of the bridge's own, which
 * stays attached to the Java runtime. The engine's callbacks arrive on the emulator's threads
 * (its loader thread among them, which comes and goes): those hand their work over and, where
 * they need an answer, wait for it.
 */
class Courier {
public:
	void Start() {
		std::scoped_lock lock{mutex_};
		if (worker_.joinable()) {
			return;
		}
		worker_ = std::thread([this] { Run(); });
	}

	/// Runs `job` on the courier's thread at some later point.
	void Post(std::function<void(JNIEnv *)> job) {
		{
			std::scoped_lock lock{mutex_};
			if (!worker_.joinable()) {
				return;
			}
			jobs_.push_back(std::move(job));
		}
		wake_.notify_one();
	}

	/// Runs `job` on the courier's thread and returns when it is done.
	void Call(const std::function<void(JNIEnv *)> &job) {
		if (std::this_thread::get_id() == worker_.get_id()) {
			job(env_);
			return;
		}
		std::mutex done_mutex;
		std::condition_variable done_wake;
		bool done = false;
		Post([&](JNIEnv *env) {
			job(env);
			{
				std::scoped_lock lock{done_mutex};
				done = true;
			}
			done_wake.notify_one();
		});
		std::unique_lock lock{done_mutex};
		done_wake.wait(lock, [&] { return done; });
	}

private:
	void Run() {
		JavaVMAttachArgs args{};
		args.version = JNI_VERSION_1_6;
		args.name = "ReaderBridge";
		if (gJvm->AttachCurrentThread(&env_, &args) != JNI_OK) {
			return;
		}
		std::unique_lock lock{mutex_};
		while (true) {
			wake_.wait(lock, [this] { return !jobs_.empty(); });
			auto job = std::move(jobs_.front());
			jobs_.pop_front();
			lock.unlock();
			job(env_);
			lock.lock();
		}
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::function<void(JNIEnv *)>> jobs_;
	std::thread worker_;
	JNIEnv *env_ = nullptr;
};

Courier s_courier;
/// Set while a profile is being loaded: only then may it ask for its data files.
thread_local bool t_loading_profile = false;

void SendBytes(jmethodID method, const char *what, const std::string &payload) {
	s_courier.Post([method, what, payload](JNIEnv *env) {
		const jbyteArray bytes = ToByteArray(env, payload);
		env->CallStaticVoidMethod(s_bridge_class, method, bytes);
		ClearException(env, what);
		env->DeleteLocalRef(bytes);
	});
}

void ReportStatus(bool loaded, std::size_t hooks, const std::string &message) {
	s_courier.Post([loaded, hooks, message](JNIEnv *env) {
		const jbyteArray text = ToByteArray(env, message);
		env->CallStaticVoidMethod(s_bridge_class, s_on_profile_status, static_cast<jboolean>(loaded), static_cast<jint>(hooks), text);
		ClearException(env, "onProfileStatus");
		env->DeleteLocalRef(text);
	});
}

/// Asks Kotlin for a file by a name: a profile's source, or one of its data files.
std::optional<std::string> AskForFile(jmethodID method, const char *what, const std::string &name) {
	std::optional<std::string> result;
	s_courier.Call([&](JNIEnv *env) {
		const jstring name_string = env->NewStringUTF(name.c_str());
		const auto data = static_cast<jbyteArray>(env->CallStaticObjectMethod(s_bridge_class, method, name_string));
		const bool threw = ClearException(env, what);
		env->DeleteLocalRef(name_string);
		if (!threw) {
			result = FromByteArray(env, data);
		}
		if (data) {
			env->DeleteLocalRef(data);
		}
	});
	return result;
}

/// Loads the running game's profile. On the emulator's thread.
void LoadProfile(const std::string &game_id) {
	auto &engine = Reader::Engine::Instance();
	if (!s_bridge_class || game_id.empty()) {
		return;
	}
	const auto source = AskForFile(s_find_profile, "findProfile", game_id);
	if (!source) {
		engine.UnloadProfile();
		ReportStatus(false, 0, "");
		return;
	}
	t_loading_profile = true;
	const bool loaded = engine.LoadProfile(game_id + ".lua", *source);
	t_loading_profile = false;
	if (loaded) {
		ReportStatus(true, engine.HookCount(), "");
	} else {
		ReportStatus(false, 0, engine.LastError());
	}
}

void Install() {
	auto &engine = Reader::Engine::Instance();
	s_courier.Start();
	engine.SetEventCallback([](const Reader::TextEvent &event) {
		SendBytes(s_on_text_event, "onTextEvent", event.ToJson());
	});
	engine.SetLayoutCallback([](const Reader::TextLayout &layout) {
		SendBytes(s_on_layout, "onLayout", layout.ToJson());
	});
	engine.SetLogCallback([](Reader::LogLevel level, const std::string &message) {
		s_courier.Post([level, message](JNIEnv *env) {
			const jbyteArray bytes = ToByteArray(env, message);
			env->CallStaticVoidMethod(s_bridge_class, s_on_log, static_cast<jint>(level), bytes);
			ClearException(env, "onLog");
			env->DeleteLocalRef(bytes);
		});
	});
	engine.SetDataProvider([](const std::string &name) -> std::optional<std::string> {
		if (!t_loading_profile) {
			return std::nullopt;
		}
		return AskForFile(s_load_profile_data, "loadProfileData", name);
	});
	Reader::Emu::SetGameLoadedCallback([](const std::string &game_id) {
		const std::string title = g_paramSFO.GetValueString("TITLE");
		s_courier.Call([&](JNIEnv *env) {
			const jstring id = env->NewStringUTF(game_id.c_str());
			const jbyteArray name = ToByteArray(env, title);
			env->CallStaticVoidMethod(s_bridge_class, s_on_game_started, id, name);
			ClearException(env, "onGameStarted");
			env->DeleteLocalRef(id);
			env->DeleteLocalRef(name);
		});
		LoadProfile(game_id);
	});
	Reader::Emu::SetGameStoppedCallback([] {
		s_courier.Post([](JNIEnv *env) {
			env->CallStaticVoidMethod(s_bridge_class, s_on_game_stopped);
			ClearException(env, "onGameStopped");
		});
	});
	Reader::Emu::SetMenuCallback([](const std::string &what) {
		SendBytes(s_on_menu, "onMenu", what);
	});
}

} // namespace
} // namespace ReaderJni

// Called when the library is loaded, so that this file is part of it (see app-android.cpp).
void ReaderJni_Link() {}

extern "C" {

JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_attach(JNIEnv *env, jobject self) {
	using namespace ReaderJni;
	if (s_bridge_class) {
		return;
	}
	const jclass bridge = env->GetObjectClass(self);
	s_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge));
	s_on_text_event = env->GetStaticMethodID(s_bridge_class, "onTextEvent", "([B)V");
	s_on_layout = env->GetStaticMethodID(s_bridge_class, "onLayout", "([B)V");
	s_on_log = env->GetStaticMethodID(s_bridge_class, "onLog", "(I[B)V");
	s_on_profile_status = env->GetStaticMethodID(s_bridge_class, "onProfileStatus", "(ZI[B)V");
	s_find_profile = env->GetStaticMethodID(s_bridge_class, "findProfile", "(Ljava/lang/String;)[B");
	s_load_profile_data = env->GetStaticMethodID(s_bridge_class, "loadProfileData", "(Ljava/lang/String;)[B");
	s_on_game_started = env->GetStaticMethodID(s_bridge_class, "onGameStarted", "(Ljava/lang/String;[B)V");
	s_on_game_stopped = env->GetStaticMethodID(s_bridge_class, "onGameStopped", "()V");
	s_on_menu = env->GetStaticMethodID(s_bridge_class, "onMenu", "([B)V");
	env->DeleteLocalRef(bridge);
	Install();
}

JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_reloadProfile(JNIEnv *, jobject) {
	// At the next frame, on the emulator's thread.
	Reader::Engine::Instance().Post([] { ReaderJni::LoadProfile(Reader::Engine::Instance().GameId()); });
}

JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_setAudioCapture(JNIEnv *, jobject, jboolean enabled, jint seconds) {
	Reader::AudioTap::Instance().SetEnabled(enabled != JNI_FALSE, static_cast<u32>(seconds > 0 ? seconds : 1));
}

JNIEXPORT jlong JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_audioPosition(JNIEnv *, jobject) {
	return static_cast<jlong>(Reader::AudioTap::Instance().Position());
}

JNIEXPORT jint JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_audioSampleRate(JNIEnv *, jobject) {
	return static_cast<jint>(Reader::AudioTap::SampleRate);
}

JNIEXPORT jshortArray JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_readAudio(JNIEnv *env, jobject, jlong start, jlong end, jlongArray actual_start) {
	u64 first = 0;
	const std::vector<s16> samples = Reader::AudioTap::Instance().Read(static_cast<u64>(start < 0 ? 0 : start), static_cast<u64>(end < 0 ? 0 : end), first);
	if (actual_start && env->GetArrayLength(actual_start) > 0) {
		const jlong value = static_cast<jlong>(first);
		env->SetLongArrayRegion(actual_start, 0, 1, &value);
	}
	const jshortArray out = env->NewShortArray(static_cast<jsize>(samples.size()));
	if (out && !samples.empty()) {
		env->SetShortArrayRegion(out, 0, static_cast<jsize>(samples.size()), samples.data());
	}
	return out;
}

JNIEXPORT jintArray JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_pictureRect(JNIEnv *env, jobject) {
	if (!PSP_IsInited() || g_display.pixel_xres <= 0 || g_display.pixel_yres <= 0) {
		return nullptr;
	}
	const DisplayLayoutConfig &config = g_Config.GetDisplayLayoutConfig(g_display.GetDeviceOrientation());
	const FRect frame{0.0f, 0.0f, static_cast<float>(g_display.pixel_xres), static_cast<float>(g_display.pixel_yres)};
	FRect rc{};
	CalculateDisplayOutputRect(config, &rc, 480.0f, 272.0f, frame, config.iInternalScreenRotation);
	const jint values[6] = {
		static_cast<jint>(g_display.pixel_xres), static_cast<jint>(g_display.pixel_yres),
		static_cast<jint>(rc.x), static_cast<jint>(rc.y),
		static_cast<jint>(rc.x + rc.w), static_cast<jint>(rc.y + rc.h),
	};
	const jintArray out = env->NewIntArray(6);
	if (out) {
		env->SetIntArrayRegion(out, 0, 6, values);
	}
	return out;
}

JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_holdSticks(JNIEnv *, jobject, jboolean held) {
	Reader::InputFilter::Instance().HoldSticks(held != JNI_FALSE);
	if (held != JNI_FALSE && PSP_IsInited()) {
		// The stick as the game last saw it would stay where it was: put it at rest.
		__CtrlSetAnalogXY(0, 0.0f, 0.0f);
	}
}

JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_pressButton(JNIEnv *, jobject, jint bit, jboolean down) {
	if (!PSP_IsInited() || bit < 0 || bit > 31) {
		return;
	}
	const u32 mask = 1u << bit;
	__CtrlUpdateButtons(down != JNI_FALSE ? mask : 0, down != JNI_FALSE ? 0 : mask);
}

/// Which answer of the choice on screen the game's cursor is on, as the profile reports it:
/// {index (-1 for none yet), 1 when the answers are side by side}, or null when it says nothing.
JNIEXPORT jintArray JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_answerCursor(JNIEnv *env, jobject) {
    const auto cursor = Reader::Engine::Instance().Cursor();
    if (!cursor) {
        return nullptr;
    }
    const jint values[2]{cursor->index, cursor->horizontal ? 1 : 0};
    const jintArray array = env->NewIntArray(2);
    if (array) {
        env->SetIntArrayRegion(array, 0, 2, values);
    }
    return array;
}

JNIEXPORT jboolean JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_inGame(JNIEnv *, jobject) {
	return PSP_IsInited() && GetUIState() == UISTATE_INGAME ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL Java_org_ppsspp_ppsspp_reader_ReaderNative_saveSyncService(JNIEnv *env, jobject) {
	const SaveSync::Host::Settings settings = SaveSync::Host::Get();
	if (!settings.enabled || settings.addresses.empty() || settings.code.empty()) {
		return nullptr;
	}
	const jclass string_class = env->FindClass("java/lang/String");
	const jobjectArray result = env->NewObjectArray(2, string_class, nullptr);
	const jstring addresses = env->NewStringUTF(settings.addresses.c_str());
	const jstring code = env->NewStringUTF(settings.code.c_str());
	env->SetObjectArrayElement(result, 0, addresses);
	env->SetObjectArrayElement(result, 1, code);
	env->DeleteLocalRef(addresses);
	env->DeleteLocalRef(code);
	env->DeleteLocalRef(string_class);
	return result;
}

} // extern "C"

// The CRT picture's settings page (reader/ui/CrtSettings.kt) reads the table of what can be
// set and writes the values, which the renderer reads.
READER_CRT_JNI(org_ppsspp_ppsspp)
