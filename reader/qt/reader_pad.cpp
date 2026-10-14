// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <QDir>
#include <QMetaObject>
#include <QSocketNotifier>
#include <QTimer>

#include "reader/qt/reader_pad.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#endif

namespace ReaderUi {

namespace {

// Valve's 2026 controller: wired, through its puck, through a Steam Machine's receiver.
[[maybe_unused]] constexpr unsigned ValveVendor = 0x28DE;
[[maybe_unused]] constexpr unsigned ValveProducts[] = {0x1302, 0x1304, 0x1305};
// Its state report, and where the buttons are in it (as SDL's SDL_hidapi_steam_triton.c and
// the Batocera bridge read it): report 0x42, a counter, then 32 bits of buttons.
[[maybe_unused]] constexpr unsigned char StateReport = 0x42;
struct GripBit {
    quint32 bit;
    int button;
};
[[maybe_unused]] constexpr GripBit GripBits[] = {
    {0x00020000, 1}, {0x00040000, 2}, {0x00000080, 3}, {0x00000100, 4}, {0x00000010, 5}};

} // namespace

struct ReaderPad::Private {
    ReaderPad* owner{};
    QTimer scan;
#if defined(__linux__) && !defined(__ANDROID__)
    struct Source {
        int fd{-1};
        QSocketNotifier* notifier{};
        /// Which device the node was when it was looked at: a controller that goes and comes
        /// back (it sleeps, it is plugged in again) often gets the same node, and is another
        /// device all the same.
        quint64 identity{};
    };

    static quint64 IdentityOf(const QString& node) {
        struct stat info {};
        if (::stat(node.toLocal8Bit().constData(), &info) != 0) {
            return 0;
        }
        return (static_cast<quint64>(info.st_ino) << 32) ^ static_cast<quint64>(info.st_ctim.tv_sec) ^
               (static_cast<quint64>(info.st_ctim.tv_nsec) << 20);
    }
    std::map<QString, Source> sources; ///< by device node

    static bool HasBit(const unsigned long* bits, int bit) {
        return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1;
    }

    void Scan() {
        const QDir input(QStringLiteral("/dev/input"));
        const QStringList nodes = input.entryList({QStringLiteral("event*")}, QDir::System);
        std::set<QString> present;
        for (const QString& name : nodes) {
            const QString node = input.filePath(name);
            present.insert(node);
            const quint64 identity = IdentityOf(node);
            if (const auto known = sources.find(node); known != sources.end()) {
                if (known->second.identity == identity) {
                    continue;
                }
                Close(known->second);
                sources.erase(known);
            }
            const int fd = ::open(node.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                continue; // not ours to read (a desktop user outside the "input" group)
            }
            unsigned long keys[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {};
            // A gamepad (it has the "south" button) with the extra buttons.
            if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !HasBit(keys, BTN_SOUTH) ||
                !HasBit(keys, BTN_TRIGGER_HAPPY1)) {
                ::close(fd);
                // Remembered as looked at, so that it is not opened again every time.
                sources[node] = Source{-1, nullptr, identity};
                continue;
            }
            auto* notifier = new QSocketNotifier(fd, QSocketNotifier::Read, owner);
            QObject::connect(notifier, &QSocketNotifier::activated, owner, [this, node] { Read(node); });
            sources[node] = Source{fd, notifier, identity};
        }
        for (auto it = sources.begin(); it != sources.end();) {
            if (present.count(it->first) == 0) {
                Close(it->second);
                it = sources.erase(it);
            } else {
                ++it;
            }
        }
    }

    static void Close(Source& source) {
        if (source.notifier != nullptr) {
            source.notifier->setEnabled(false);
            source.notifier->deleteLater();
            source.notifier = nullptr;
        }
        if (source.fd >= 0) {
            ::close(source.fd);
            source.fd = -1;
        }
    }

    void Read(const QString& node) {
        const auto found = sources.find(node);
        if (found == sources.end() || found->second.fd < 0) {
            return;
        }
        input_event events[32];
        for (;;) {
            const ssize_t got = ::read(found->second.fd, events, sizeof(events));
            if (got < static_cast<ssize_t>(sizeof(input_event))) {
                if (got < 0 && errno != EAGAIN && errno != EINTR) {
                    // unplugged: forgotten, so that whatever takes the node next is looked at
                    Close(found->second);
                    sources.erase(found);
                }
                return;
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(got) / sizeof(input_event); ++i) {
                const input_event& event = events[i];
                if (event.type == EV_KEY && event.value == 1 && event.code >= BTN_TRIGGER_HAPPY1 &&
                    event.code < BTN_TRIGGER_HAPPY1 + ReaderPad::Buttons) {
                    emit owner->Pressed(event.code - BTN_TRIGGER_HAPPY1 + 1);
                }
            }
        }
    }

    int Count() const {
        int count = 0;
        for (const auto& [node, source] : sources) {
            count += source.fd >= 0 ? 1 : 0;
        }
        return count;
    }

    void Stop() {
        for (auto& [node, source] : sources) {
            Close(source);
        }
        sources.clear();
    }
#elif defined(_WIN32)
    // One thread per controller found, each waiting on the device's reports. A thread ends
    // when its controller goes or when the reader does.
    struct Listener {
        std::thread thread;
        HANDLE device{INVALID_HANDLE_VALUE};
        HANDLE stop{nullptr};
        std::atomic<bool> gone{false};
    };
    std::map<std::wstring, std::unique_ptr<Listener>> listeners; ///< by device path

    void Scan() {
        for (auto it = listeners.begin(); it != listeners.end();) {
            if (it->second->gone.load()) {
                End(*it->second);
                it = listeners.erase(it);
            } else {
                ++it;
            }
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
            // Shared with whoever else reads the controller; nothing is ever written.
            const HANDLE device = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
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
            ReaderPad* pad = owner;
            listener->thread = std::thread([raw, pad] { Listen(*raw, pad); });
            listeners[path] = std::move(listener);
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    /// On the listener's own thread. Says a press to the reader's thread and touches nothing
    /// else of it.
    static void Listen(Listener& listener, ReaderPad* pad) {
        OVERLAPPED pending{};
        pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        unsigned char report[128];
        quint32 before = 0;
        bool seen = false;
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
                    break; // the reader is ending
                }
                if (!GetOverlappedResult(listener.device, &pending, &got, FALSE)) {
                    break;
                }
            }
            if (got < 6 || report[0] != StateReport) {
                continue; // another interface of the controller, or another report
            }
            const quint32 buttons = static_cast<quint32>(report[2]) | (static_cast<quint32>(report[3]) << 8) |
                                    (static_cast<quint32>(report[4]) << 16) |
                                    (static_cast<quint32>(report[5]) << 24);
            if (seen) {
                for (const GripBit& grip : GripBits) {
                    if ((buttons & grip.bit) != 0 && (before & grip.bit) == 0) {
                        const int button = grip.button;
                        QMetaObject::invokeMethod(pad, [pad, button] { emit pad->Pressed(button); },
                                                  Qt::QueuedConnection);
                    }
                }
            }
            seen = true;
            before = buttons;
        }
        CloseHandle(pending.hEvent);
        listener.gone.store(true);
    }

    static void End(Listener& listener) {
        SetEvent(listener.stop);
        if (listener.thread.joinable()) {
            listener.thread.join();
        }
        CloseHandle(listener.device);
        CloseHandle(listener.stop);
    }

    int Count() const {
        int count = 0;
        for (const auto& [path, listener] : listeners) {
            count += listener->gone.load() ? 0 : 1;
        }
        return count;
    }

    void Stop() {
        for (auto& [path, listener] : listeners) {
            End(*listener);
        }
        listeners.clear();
    }
#else
    void Scan() {}
    int Count() const {
        return 0;
    }
    void Stop() {}
#endif
};

ReaderPad::ReaderPad(QObject* parent) : QObject(parent), d{std::make_unique<Private>()} {
    d->owner = this;
    // Controllers come and go: a look every few seconds finds a new one.
    d->scan.setInterval(1500);
    connect(&d->scan, &QTimer::timeout, this, [this] { d->Scan(); });
    d->scan.start();
    QTimer::singleShot(0, this, [this] { d->Scan(); });
}

ReaderPad::~ReaderPad() {
    d->scan.stop();
    d->Stop();
}

void ReaderPad::Inject(int button) {
    if (button >= 1 && button <= Buttons) {
        emit Pressed(button);
    }
}

int ReaderPad::Sources() const {
    return d->Count();
}

} // namespace ReaderUi
