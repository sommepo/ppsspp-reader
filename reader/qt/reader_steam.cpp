// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <QJsonObject>
#include <QTimer>

#include "reader/desktop/steam_bridge.h"
#include "reader/qt/reader_link.h"
#include "reader/qt/reader_steam.h"

// The emulator's own input library, whichever generation of it the emulator uses.
// The build says which (READER_SDL_MAJOR); without its word, the newer one where its headers
// are there.
#if !defined(READER_SDL_MAJOR)
#if __has_include(<SDL3/SDL.h>)
#define READER_SDL_MAJOR 3
#else
#define READER_SDL_MAJOR 2
#endif
#endif
#if READER_SDL_MAJOR == 3
#include <SDL3/SDL.h>
#define READER_STEAM_SDL3 1
#else
#include <SDL.h>
#define READER_STEAM_SDL3 0
#endif

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#include <tlhelp32.h>
extern "C" {
#include <hidsdi.h>
}
#endif

namespace ReaderUi {

namespace Bridge = ReaderApp::SteamBridge;

namespace {

constexpr unsigned ValveVendor = 0x28DE;
[[maybe_unused]] constexpr unsigned ValveProducts[] = {0x1302, 0x1304, 0x1305};

/// Where the controller's doings go: a gamepad inside the program, and the system's mouse.
/// Called from the thread that reads the controller.
class SystemSink final : public Bridge::Sink {
public:
    ~SystemSink() override {
        Detach();
    }

    void PadButton(Bridge::Button button, bool down) override {
        std::scoped_lock lock{mutex};
        if (!Attach()) {
            return;
        }
        SDL_LockJoysticks();
#if READER_STEAM_SDL3
        SDL_SetJoystickVirtualButton(joystick, static_cast<int>(button), down);
#else
        SDL_JoystickSetVirtualButton(joystick, static_cast<int>(button), down ? 1 : 0);
#endif
        SDL_UnlockJoysticks();
        ++pad_events;
    }

    void PadAxis(Bridge::Axis axis, int value) override {
        std::scoped_lock lock{mutex};
        if (!Attach()) {
            return;
        }
        SDL_LockJoysticks();
#if READER_STEAM_SDL3
        SDL_SetJoystickVirtualAxis(joystick, static_cast<int>(axis), static_cast<Sint16>(value));
#else
        SDL_JoystickSetVirtualAxis(joystick, static_cast<int>(axis), static_cast<Sint16>(value));
#endif
        SDL_UnlockJoysticks();
        ++pad_events;
    }

    void MouseMove(int dx, int dy) override {
        mouse_x += dx;
        mouse_y += dy;
#if defined(_WIN32)
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
#endif
    }

    void MouseClick(Bridge::MouseButton button, bool down) override {
        ++mouse_clicks;
#if defined(_WIN32)
        INPUT input{};
        input.type = INPUT_MOUSE;
        if (button == Bridge::MouseButton::Left) {
            input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        } else {
            input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        }
        SendInput(1, &input, sizeof(input));
#else
        (void)button;
        (void)down;
#endif
    }

    void MouseWheel(int amount) override {
        mouse_wheel += amount;
#if defined(_WIN32)
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(amount);
        SendInput(1, &input, sizeof(input));
#endif
    }

    [[nodiscard]] bool Attached() const {
        return joystick != nullptr;
    }

    std::atomic<int> pad_events{0};
    std::atomic<int> mouse_x{0}, mouse_y{0}, mouse_clicks{0}, mouse_wheel{0};

private:
    /// Makes the gamepad once the emulator's input library is up. False until then.
    bool Attach() {
        if (joystick != nullptr) {
            return true;
        }
#if READER_STEAM_SDL3
        if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
            return false;
        }
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = Bridge::AxisCount;
        desc.nbuttons = Bridge::ButtonCount;
        desc.vendor_id = ValveVendor;
        desc.product_id = 0x1302;
        desc.button_mask = (1u << Bridge::ButtonCount) - 1;
        desc.axis_mask = (1u << Bridge::AxisCount) - 1;
        desc.name = "Steam Controller";
        id = SDL_AttachVirtualJoystick(&desc);
        if (id == 0) {
            return false;
        }
        joystick = SDL_OpenJoystick(id);
#else
        if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
            return false;
        }
        SDL_VirtualJoystickDesc desc;
        SDL_zero(desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = Bridge::AxisCount;
        desc.nbuttons = Bridge::ButtonCount;
        desc.vendor_id = ValveVendor;
        desc.product_id = 0x1302;
        desc.button_mask = (1u << Bridge::ButtonCount) - 1;
        desc.axis_mask = (1u << Bridge::AxisCount) - 1;
        desc.name = "Steam Controller";
        index = SDL_JoystickAttachVirtualEx(&desc);
        if (index < 0) {
            return false;
        }
        joystick = SDL_JoystickOpen(index);
#endif
        return joystick != nullptr;
    }

    void Detach() {
        std::scoped_lock lock{mutex};
        if (joystick == nullptr) {
            return;
        }
#if READER_STEAM_SDL3
        SDL_CloseJoystick(joystick);
        SDL_DetachVirtualJoystick(id);
#else
        SDL_JoystickClose(joystick);
        SDL_JoystickDetachVirtual(index);
#endif
        joystick = nullptr;
    }

    std::mutex mutex;
    SDL_Joystick* joystick{};
#if READER_STEAM_SDL3
    SDL_JoystickID id{};
#else
    int index{-1};
#endif
};

} // namespace

struct SteamControllerBridge::Private {
    SystemSink sink;
    Bridge::Pad injected; ///< The controller the development channel plays

    // How the pads feel: Reader Home's sliders (the shared settings' "steam"), read again
    // when that file changes. The listeners take a copy when the count has moved.
    std::mutex tuning_mutex;
    Bridge::Tuning tuning;
    std::atomic<int> tuning_count{0};

    void ReadTuning() {
        bool changed = false;
        const QJsonObject steam = HomeLink::SharedSettings(&changed).value(QStringLiteral("steam")).toObject();
        if (!changed) {
            return;
        }
        const auto number = [&](const char* key, double otherwise, double low, double high) {
            return std::clamp(steam.value(QLatin1String(key)).toDouble(otherwise), low, high);
        };
        Bridge::Tuning made;
        made.swipe_pixels *= number("sensitivity", 1.0, 0.2, 4.0);
        made.acceleration = number("acceleration", made.acceleration, 0.0, 3.0);
        made.scroll_notches *= number("scroll", 1.0, 0.2, 4.0);
        made.trackball = steam.value(QStringLiteral("trackball")).toBool(true);
        made.haptics = steam.value(QStringLiteral("haptics")).toBool(true);
        made.tick_micros = static_cast<int>(made.tick_micros * number("tick_strength", 1.0, 0.0, 4.0));
        made.click_micros = static_cast<int>(made.click_micros * number("click_strength", 1.0, 0.0, 4.0));
        {
            std::lock_guard lock{tuning_mutex};
            tuning = made;
        }
        injected.SetTuning(made);
        ++tuning_count;
    }
    /// For a listener: the tuning when it is newer than the one it has.
    void TakeTuning(Bridge::Pad& pad, int& have) {
        const int now = tuning_count.load();
        if (now != have) {
            have = now;
            std::lock_guard lock{tuning_mutex};
            pad.SetTuning(tuning);
        }
    }
    std::atomic<int> injected_pulses{0};

    /// How many ticks went to the controllers, and what became of them.
    QString PulsesText() {
        int sent = injected_pulses.load(), failed = 0;
        unsigned long error = 0;
#if defined(_WIN32)
        for (auto& [path, listener] : listeners) {
            sent += listener->pulses_sent.load();
            failed += listener->pulses_failed.load();
            if (listener->pulse_error.load() != 0) {
                error = listener->pulse_error.load();
            }
        }
#endif
        return failed == 0 ? QString::number(sent)
                           : QStringLiteral("%1 (%2 refused, error %3)").arg(sent).arg(failed).arg(error);
    }
    QTimer scan;
    std::atomic<int> controllers{0};
    std::atomic<bool> steam_runs{false};

#if defined(_WIN32)
    struct Listener {
        std::thread thread;
        HANDLE device{INVALID_HANDLE_VALUE};
        HANDLE stop{nullptr};
        std::atomic<bool> gone{false};
        DWORD output_length{};               ///< Of an output report; 0 until asked
        std::atomic<int> pulses_sent{0}, pulses_failed{0};
        std::atomic<DWORD> pulse_error{0};
    };
    std::map<std::wstring, std::unique_ptr<Listener>> listeners; ///< by device path

    static bool SteamRuns() {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            if (lstrcmpiW(entry.szExeFile, L"steam.exe") == 0) {
                found = true;
                break;
            }
        }
        CloseHandle(snapshot);
        return found;
    }

    void Scan() {
        for (auto it = listeners.begin(); it != listeners.end();) {
            if (it->second->gone.load()) {
                End(*it->second);
                it = listeners.erase(it);
            } else {
                ++it;
            }
        }
        // With Steam running the controller is Steam's: it is left alone.
        steam_runs.store(SteamRuns());
        if (steam_runs.load()) {
            Stop();
            return;
        }
        GUID hid;
        HidD_GetHidGuid(&hid);
        const HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) {
            return;
        }
        SP_DEVICE_INTERFACE_DATA item{};
        item.cbSize = sizeof(item);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, index, &item); ++index) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &needed, nullptr);
            if (needed == 0) {
                continue;
            }
            std::vector<char> buffer(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &item, detail, needed, nullptr, nullptr)) {
                continue;
            }
            const std::wstring path = detail->DevicePath;
            if (listeners.count(path) != 0) {
                continue;
            }
            const HANDLE device = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                              FILE_FLAG_OVERLAPPED, nullptr);
            if (device == INVALID_HANDLE_VALUE) {
                continue;
            }
            HIDD_ATTRIBUTES attributes{};
            attributes.Size = sizeof(attributes);
            bool ours = false;
            if (HidD_GetAttributes(device, &attributes) && attributes.VendorID == ValveVendor) {
                for (const unsigned product : ValveProducts) {
                    ours = ours || attributes.ProductID == product;
                }
            }
            if (!ours) {
                CloseHandle(device);
                continue;
            }
            auto listener = std::make_unique<Listener>();
            listener->device = device;
            listener->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Listener* raw = listener.get();
            listener->thread = std::thread([this, raw] { Listen(*raw); });
            listeners[path] = std::move(listener);
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    /// On the listener's own thread: one of the controller's interfaces. The one that sends
    /// state reports is the controller; the others stay silent and cost nothing.
    void Listen(Listener& listener) {
        Bridge::Pad pad;
        int tuning_have = -1;
        OVERLAPPED pending{};
        pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        unsigned char report[256];
        bool counted = false;
        ULONGLONG lizard_at = 0;
        for (;;) {
            DWORD got = 0;
            ResetEvent(pending.hEvent);
            if (!ReadFile(listener.device, report, sizeof(report), &got, &pending)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    break; // the controller is gone
                }
                const HANDLE either[] = {pending.hEvent, listener.stop};
                if (WaitForMultipleObjects(2, either, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    CancelIo(listener.device);
                    break; // the reader is ending, or Steam has started
                }
                if (!GetOverlappedResult(listener.device, &pending, &got, FALSE)) {
                    break;
                }
            }
            TakeTuning(pad, tuning_have);
            if (!pad.Report(report, got, sink)) {
                continue;
            }
            Bridge::Pulse pulse{};
            while (pad.TakePulse(pulse)) {
                Haptic(listener, pulse);
            }
            if (!counted) {
                counted = true;
                ++controllers;
            }
            // The controller's own keyboard and mouse are switched off, again and again: its
            // firmware switches them back on after a few seconds.
            const ULONGLONG now = GetTickCount64();
            if (now - lizard_at >= 2000) {
                lizard_at = now;
                unsigned char feature[Bridge::FeatureLength + 1] = {};
                for (std::size_t i = 0; i < Bridge::LizardOff.size(); ++i) {
                    feature[i] = Bridge::LizardOff[i];
                }
                HidD_SetFeature(listener.device, feature, static_cast<ULONG>(Bridge::FeatureLength));
            }
        }
        pad.Release(sink);
        if (counted) {
            --controllers;
        }
        CloseHandle(pending.hEvent);
        listener.gone.store(true);
    }

    /// A tick in one of the controller's pads: an output report, of the length the
    /// controller says its output reports have.
    static void Haptic(Listener& listener, const Bridge::Pulse& pulse) {
        if (listener.output_length == 0) {
            listener.output_length = 64;
            PHIDP_PREPARSED_DATA parsed = nullptr;
            if (HidD_GetPreparsedData(listener.device, &parsed)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS && caps.OutputReportByteLength >= Bridge::HapticLength) {
                    listener.output_length = caps.OutputReportByteLength;
                }
                HidD_FreePreparsedData(parsed);
            }
        }
        unsigned char message[512] = {};
        const auto bytes = Bridge::HapticMessage(pulse);
        std::copy(bytes.begin(), bytes.end(), message);
        const DWORD length = std::min<DWORD>(listener.output_length, sizeof(message));
        OVERLAPPED done{};
        done.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        bool sent = WriteFile(listener.device, message, length, &written, &done) != 0;
        if (!sent && GetLastError() == ERROR_IO_PENDING) {
            // A tick is not worth waiting long for.
            if (WaitForSingleObject(done.hEvent, 20) == WAIT_OBJECT_0) {
                sent = GetOverlappedResult(listener.device, &done, &written, FALSE) != 0;
            } else {
                CancelIoEx(listener.device, &done);
                GetOverlappedResult(listener.device, &done, &written, TRUE);
            }
        }
        CloseHandle(done.hEvent);
        if (sent) {
            ++listener.pulses_sent;
        } else if (HidD_SetOutputReport(listener.device, message, length)) {
            ++listener.pulses_sent; // some connections take it only this way
        } else {
            ++listener.pulses_failed;
            listener.pulse_error = GetLastError();
        }
    }

    static void End(Listener& listener) {
        SetEvent(listener.stop);
        if (listener.thread.joinable()) {
            listener.thread.join();
        }
        CloseHandle(listener.device);
        CloseHandle(listener.stop);
    }

    void Stop() {
        for (auto& [path, listener] : listeners) {
            End(*listener);
        }
        listeners.clear();
    }
#else
    void Scan() {}
    void Stop() {}
#endif
};

SteamControllerBridge::SteamControllerBridge(QObject* parent)
    : QObject(parent), d{std::make_unique<Private>()} {
    // Every three seconds: the sliders' file when it has changed, and (on Windows) the
    // controllers that came and went.
    d->scan.setInterval(3000);
    connect(&d->scan, &QTimer::timeout, this, [this] {
        d->ReadTuning();
        d->Scan();
    });
    d->ReadTuning();
    d->scan.start();
    QTimer::singleShot(1500, this, [this] { d->Scan(); });
}

SteamControllerBridge::~SteamControllerBridge() {
    d->scan.stop();
    d->Stop();
}

bool SteamControllerBridge::InjectReport(const QByteArray& report) {
    const bool taken = d->injected.Report(reinterpret_cast<const std::uint8_t*>(report.constData()),
                                          static_cast<std::size_t>(report.size()), d->sink);
    // A played report has no controller to tick: the ticks are counted.
    Bridge::Pulse pulse{};
    while (d->injected.TakePulse(pulse)) {
        ++d->injected_pulses;
    }
    return taken;
}

QString SteamControllerBridge::Describe() const {
    return QStringLiteral("controllers %1, Steam running %2, gamepad made %3, gamepad events %4, "
                          "mouse moved %5 %6, clicks %7, wheel %8, pad ticks %9")
        .arg(d->controllers.load())
        .arg(d->steam_runs.load() ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(d->sink.Attached() ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(d->sink.pad_events.load())
        .arg(d->sink.mouse_x.load())
        .arg(d->sink.mouse_y.load())
        .arg(d->sink.mouse_clicks.load())
        .arg(d->sink.mouse_wheel.load())
        .arg(d->PulsesText());
}

} // namespace ReaderUi
