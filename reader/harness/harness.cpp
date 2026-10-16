// SPDX-FileCopyrightText: PPSSPP Reader fork contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The headless harness: runs a PSP game on a machine with no display, GPU or sound device and
// takes text commands on a socket. It is how text profiles are written and how the text engine
// is tried against a real game. See docs/reader/harness.md.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "Common/CPUDetect.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/DirectoryReader.h"
#include "Common/File/VFS/VFS.h"
#include "Common/GraphicsContext.h"
#include "Common/Log/LogManager.h"
#include "Common/Profiler/Profiler.h"
#include "Common/System/NativeApp.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Breakpoints.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceUtility.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MemMap.h"
#include "Core/SaveState.h"
#include "Core/Screenshot.h"
#include "Core/System.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/GPUCommon.h"

#include "reader/core/audio_tap.h"
#include "reader/core/emulator.h"
#include "reader/core/engine.h"
#include "reader/core/text_codec.h"

// ---- What a frontend owes the emulator, answered with nothing ----

void NativeFrame(GraphicsContext* graphicsContext) {}
void NativeResized() {}
std::string System_GetProperty(SystemProperty prop) {
    return "";
}
std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) {
    return {};
}
int64_t System_GetPropertyInt(SystemProperty prop) {
    return prop == SYSPROP_SYSTEMVERSION ? 31 : -1;
}
float System_GetPropertyFloat(SystemProperty prop) {
    return -1.0f;
}
bool System_GetPropertyBool(SystemProperty prop) {
    return prop == SYSPROP_CAN_JIT || prop == SYSPROP_SKIP_UI;
}
void System_Notify(SystemNotification notification) {}
void System_PostUIMessage(UIMessage message, std::string_view param) {}
void System_RunOnMainThread(std::function<void()>) {}
bool System_MakeRequest(SystemRequestType type, int requestId, const std::string& param1,
                        const std::string& param2, int64_t param3, int64_t param4) {
    return false;
}
void System_AskForPermission(SystemPermission permission) {}
PermissionStatus System_GetPermissionStatus(SystemPermission permission) {
    return PERMISSION_STATUS_GRANTED;
}
void System_AudioGetDebugStats(char* buf, size_t bufSize) {
    if (buf) {
        buf[0] = '\0';
    }
}
void System_AudioClear() {}
void System_AudioPushSamples(const s32* audio, int numSamples, float volume) {}
bool NativeSaveSecret(std::string_view nameOfSecret, std::string_view data) {
    return false;
}
std::string NativeLoadSecret(std::string_view nameOfSecret) {
    return "";
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr u32 ScreenWidth = 480;
constexpr u32 ScreenHeight = 272;

const std::map<std::string, u32> ButtonNames{
    {"select", 0x0001}, {"start", 0x0008},    {"up", 0x0010},     {"right", 0x0020},
    {"down", 0x0040},   {"left", 0x0080},     {"l", 0x0100},      {"r", 0x0200},
    {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000},  {"square", 0x8000},
};

struct Reply {
    std::string body;
    bool ok = true;
    std::string error;

    void Fail(std::string message) {
        ok = false;
        error = std::move(message);
    }
    std::string Format() const {
        std::string out = body;
        if (!out.empty() && out.back() != '\n') {
            out.push_back('\n');
        }
        out += ok ? "OK\n" : "ERR " + error + "\n";
        return out;
    }
};

std::string ReadFile(const std::filesystem::path& path, bool& ok) {
    std::ifstream file{path, std::ios::binary};
    ok = static_cast<bool>(file);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string BytesFromHex(const std::string& hex) {
    std::string bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

/// UTF-8 to Shift-JIS, by the decoder's own table turned round. Empty when a character has no
/// place in it.
std::string ToSjis(const std::string& utf8) {
    static const std::map<std::string, std::string> table = [] {
        std::map<std::string, std::string> map;
        for (u32 lead = 0x81; lead <= 0xFC; ++lead) {
            if (lead >= 0xA0 && lead <= 0xDF) {
                continue;
            }
            for (u32 trail = 0x40; trail <= 0xFC; ++trail) {
                const u8 pair[2]{static_cast<u8>(lead), static_cast<u8>(trail)};
                const std::string decoded = Reader::SjisToUtf8(pair);
                if (decoded != "\xEF\xBF\xBD" && Reader::CountCodePoints(decoded) == 1) {
                    map.emplace(decoded, std::string(reinterpret_cast<const char*>(pair), 2));
                }
            }
        }
        for (u32 single = 0xA1; single <= 0xDF; ++single) {
            const u8 one[1]{static_cast<u8>(single)};
            map.emplace(Reader::SjisToUtf8(one), std::string(1, static_cast<char>(single)));
        }
        return map;
    }();
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const u8 lead = static_cast<u8>(utf8[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        const std::string one = utf8.substr(i, length);
        i += length;
        if (length == 1) {
            out += one;
            continue;
        }
        const auto it = table.find(one);
        if (it == table.end()) {
            return {};
        }
        out += it->second;
    }
    return out;
}

std::string ToUtf16(const std::string& utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const u8 lead = static_cast<u8>(utf8[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        u32 cp = length == 1 ? lead : lead & (0xFF >> (length + 1));
        for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
            cp = (cp << 6) | (static_cast<u8>(utf8[i + k]) & 0x3F);
        }
        i += length;
        const auto put = [&out](u32 unit) {
            out.push_back(static_cast<char>(unit & 0xFF));
            out.push_back(static_cast<char>(unit >> 8));
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put(0xD800 + (cp >> 10));
            put(0xDC00 + (cp & 0x3FF));
        } else {
            put(cp);
        }
    }
    return out;
}

class Harness {
public:
    std::atomic<bool> quit{false};

    bool Boot(const std::filesystem::path& rom, const std::filesystem::path& home,
              const std::filesystem::path& assets, bool interpreter) {
        g_Config.bEnableLogging = true;
        g_logManager.Init(&g_Config.bEnableLogging, false);
        for (int i = 0; i < static_cast<int>(Log::NUMBER_OF_LOGS); i++) {
            g_logManager.SetEnabled(static_cast<Log>(i), true);
            g_logManager.SetLogLevel(static_cast<Log>(i), LogLevel::LERROR);
        }
        if (std::getenv("READER_HARNESS_LOG")) {
            for (int i = 0; i < static_cast<int>(Log::NUMBER_OF_LOGS); i++) {
                g_logManager.SetLogLevel(static_cast<Log>(i), LogLevel::LINFO);
            }
            g_logManager.EnableOutput(LogOutput::Printf);
        }
        g_threadManager.Init(cpu_info.num_cores, cpu_info.logical_cpu_count);

        CoreParameter param;
        param.cpuCore = interpreter ? CPUCore::INTERPRETER : CPUCore::JIT;
        param.gpuCore = GPUCORE_SOFTWARE;
        param.graphicsContext = nullptr;
        param.enableSound = false;
        param.fileToStart = Path(rom.string());
        param.startBreak = false;
        param.headLess = true;
        param.renderScaleFactor = 1;
        param.renderWidth = ScreenWidth;
        param.renderHeight = ScreenHeight;
        param.pixelWidth = ScreenWidth;
        param.pixelHeight = ScreenHeight;
        param.fastForward = true;

        g_Config.RestoreDefaults(RestoreSettingsBits::SETTINGS | RestoreSettingsBits::CONTROLS,
                                 true);
        g_Config.iCpuCore = static_cast<int>(param.cpuCore);
        g_Config.iDumpFileTypes = 0;
        g_Config.bEnableSound = false;
        g_Config.bFirstRun = false;
        g_Config.sReportHost.clear();
        g_Config.bAutoSaveSymbolMap = false;
        g_Config.bSoftwareRendering = true;
        g_Config.bSoftwareRenderingJit = true;
        g_Config.iInternalResolution = 1;
        // A Japanese console: the game speaks Japanese and takes circle for "yes".
        g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;
        g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CIRCLE;
        g_Config.iTimeFormat = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
        g_Config.sNickName = "reader";
        g_Config.bEncryptSave = true;
        g_Config.bMemStickInserted = true;
        g_Config.iMemStickSizeGB = 16;
        g_Config.iFirmwareVersion = PSP_DEFAULT_FIRMWARE;
        g_Config.iPSPModel = PSP_MODEL_SLIM;
        g_Config.internalDataDirectory.clear();
        g_Config.bEnableLogging = true;

        std::filesystem::create_directories(home / "memstick");
        g_Config.memStickDirectory = Path((home / "memstick").string());
        g_Config.flash0Directory = Path((assets / "flash0").string());
        g_VFS.Register("", new DirectoryReader(Path(assets.string())));
        CreateSysDirectories();

        UpdateUIState(UISTATE_INGAME);

        auto& engine = Reader::Engine::Instance();
        engine.SetEventCallback([this](const Reader::TextEvent& event) {
            std::scoped_lock lock{output_mutex};
            events.push_back(event.ToJson());
        });
        engine.SetLayoutCallback([this](const Reader::TextLayout& layout) {
            std::scoped_lock lock{output_mutex};
            layouts.push_back(layout.ToJson());
        });
        engine.SetLogCallback([this](Reader::LogLevel level, const std::string& message) {
            std::scoped_lock lock{output_mutex};
            const char* prefix = level == Reader::LogLevel::Error     ? "error: "
                                 : level == Reader::LogLevel::Warning ? "warning: "
                                                                      : "";
            logs.push_back(std::string("[reader] ") + prefix + message);
        });
        engine.SetDataProvider([this](const std::string& name) -> std::optional<std::string> {
            std::filesystem::path dir;
            {
                std::scoped_lock lock{output_mutex};
                dir = profile_dir;
            }
            bool ok = false;
            const std::string id = Reader::Engine::Instance().GameId();
            std::string data = ReadFile(dir / (id + "." + name), ok);
            if (!ok) {
                return std::nullopt;
            }
            return data;
        });

        std::string error;
        if (!PSP_InitStart(param)) {
            std::cerr << "cannot start " << rom << "\n";
            return false;
        }
        while (PSP_InitUpdate(&error) == BootState::Booting) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!PSP_IsInited()) {
            std::cerr << "cannot start " << rom << ": " << error << "\n";
            return false;
        }
        System_Notify(SystemNotification::BOOT_DONE);
        if (gpu) {
            gpu->BeginHostFrame(g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape));
        }
        coreState = CORE_RUNNING_CPU;
        booted = true;
        return true;
    }

    void Shutdown() {
        if (!booted) {
            return;
        }
        if (gpu) {
            gpu->EndHostFrame();
        }
        PSP_Shutdown(true);
        booted = false;
        g_VFS.Clear();
        g_threadManager.Teardown();
    }

    Reply Execute(const std::string& line) {
        Reply reply;
        std::istringstream in{line};
        std::string command;
        in >> command;
        const auto rest = [&in] {
            std::string text;
            std::getline(in >> std::ws, text);
            return text;
        };
        auto& engine = Reader::Engine::Instance();

        try {
            if (command.empty() || command[0] == '#') {
                // blank line or comment
            } else if (command == "run") {
                // run <frames>
                u64 count = 1;
                in >> count;
                Run(count, reply);
            } else if (command == "press") {
                // press <button> [hold=4] [after=4]
                std::string name;
                u64 hold = 4;
                u64 after = 4;
                in >> name >> hold >> after;
                const auto it = ButtonNames.find(name);
                if (it == ButtonNames.end()) {
                    reply.Fail("unknown button " + name);
                } else {
                    __CtrlUpdateButtons(it->second, 0);
                    Run(hold, reply);
                    __CtrlUpdateButtons(0, it->second);
                    Run(after, reply);
                }
            } else if (command == "hold" || command == "release") {
                std::string name;
                in >> name;
                const auto it = ButtonNames.find(name);
                if (it == ButtonNames.end()) {
                    reply.Fail("unknown button " + name);
                } else if (command == "hold") {
                    __CtrlUpdateButtons(it->second, 0);
                } else {
                    __CtrlUpdateButtons(0, it->second);
                }
            } else if (command == "stick") {
                // stick <x> <y>, each from -1 to 1 (up is positive)
                float x = 0.0f;
                float y = 0.0f;
                in >> x >> y;
                __CtrlSetAnalogXY(0, std::clamp(x, -1.0f, 1.0f), std::clamp(y, -1.0f, 1.0f));
            } else if (command == "shot") {
                Screenshot(rest(), reply);
            } else if (command == "cursor") {
                // Which answer of the choice on screen the profile says the cursor is on.
                const auto cursor = Reader::Engine::Instance().Cursor();
                reply.body += cursor ? std::to_string(cursor->index) + (cursor->horizontal ? " horizontal" : "") + "\n"
                                     : std::string("none\n");
            } else if (command == "events") {
                std::scoped_lock lock{output_mutex};
                for (const auto& event : events) {
                    reply.body += event + "\n";
                }
                events.clear();
            } else if (command == "layouts") {
                std::scoped_lock lock{output_mutex};
                for (const auto& layout : layouts) {
                    reply.body += layout + "\n";
                }
                layouts.clear();
            } else if (command == "profile") {
                LoadProfile(rest(), reply);
            } else if (command == "unload") {
                engine.UnloadProfile();
            } else if (command == "lua") {
                std::string output;
                const bool ok = engine.Execute(rest(), output);
                reply.body += output;
                if (!ok) {
                    reply.Fail("lua error");
                }
            } else if (command == "mem") {
                // mem <hex address> [length=64]
                u64 address = 0;
                std::size_t length = 64;
                in >> std::hex >> address >> std::dec >> length;
                HexDump(address, std::min<std::size_t>(length, 1 << 16), reply);
            } else if (command == "poke") {
                // poke <hex address> <hex bytes>
                u64 address = 0;
                std::string hex;
                in >> std::hex >> address >> hex;
                const std::string bytes = BytesFromHex(hex);
                if (bytes.empty() || !Memory::IsValidRange(static_cast<u32>(address),
                                                           static_cast<u32>(bytes.size()))) {
                    reply.Fail("cannot write there");
                } else {
                    std::memcpy(Memory::GetPointerWriteUnchecked(static_cast<u32>(address)),
                                bytes.data(), bytes.size());
                }
            } else if (command == "memfile") {
                // memfile <hex address> <length> <path>
                u64 address = 0;
                std::size_t length = 0;
                in >> std::hex >> address >> std::dec >> length;
                MemoryToFile(address, length, rest(), reply);
            } else if (command == "find") {
                // find <hex bytes> [hex start] [hex end]
                std::string hex;
                u64 start = 0;
                u64 end = ~0ULL;
                in >> hex >> std::hex >> start >> end;
                Find(BytesFromHex(hex), start, end, reply);
            } else if (command == "findtext") {
                // findtext <text>: as Shift-JIS, as UTF-8 and as UTF-16
                const std::string text = rest();
                const std::string sjis = ToSjis(text);
                reply.body += "Shift-JIS:\n";
                if (!sjis.empty()) {
                    Find(sjis, 0, ~0ULL, reply);
                }
                reply.body += "UTF-8:\n";
                Find(text, 0, ~0ULL, reply);
                reply.body += "UTF-16:\n";
                Find(ToUtf16(text), 0, ~0ULL, reply);
            } else if (command == "findptr") {
                // findptr <hex low> <hex high>: where memory holds a pointer into [low, high)
                u64 low = 0;
                u64 high = 0;
                in >> std::hex >> low >> high;
                FindPointers(low, high, reply);
            } else if (command == "maps") {
                static const char* const names[] = {"?", "kernel", "user", "vram", "scratchpad"};
                for (const auto& region : engine.MemoryRegions()) {
                    reply.body += std::format("{:08x} {:8x} {}\n", region.base, region.size,
                                              names[std::min<u32>(region.state, 4)]);
                }
            } else if (command == "modules") {
                for (const auto& module : engine.Modules()) {
                    reply.body += std::format("{:<16} {:08x} {:8x} {}\n", module.name,
                                              module.base, module.size, module.build_id);
                }
            } else if (command == "regs") {
                for (int i = 0; i < 32; ++i) {
                    reply.body += std::format("r{:<2}={:08x}{}", i, currentMIPS->r[i],
                                              i % 4 == 3 ? "\n" : " ");
                }
                reply.body += std::format("pc={:08x}\n", currentMIPS->pc);
            } else if (command == "disasm") {
                // disasm <hex address> [instructions=24]
                u64 address = 0;
                u32 count = 24;
                in >> std::hex >> address >> std::dec >> count;
                for (u32 i = 0; i < std::min<u32>(count, 4096); ++i) {
                    const u32 pc = static_cast<u32>(address) + i * 4;
                    if (!Memory::IsValidAddress(pc)) {
                        break;
                    }
                    char text[256];
                    MIPSDisAsm(Memory::Read_Instruction(pc, true), pc, text, sizeof(text), true);
                    reply.body += std::format("{:08x}: {:08x}  {}\n", pc, Memory::Read_U32(pc), text);
                }
            } else if (command == "watch") {
                // watch r|w|rw <hex address> [bytes=4] [frames=1]: which code reads or writes
                // this memory, with how often and (the first time) from where it was called
                std::string mode;
                u64 address = 0;
                u32 length = 4;
                u64 frames = 1;
                in >> mode >> std::hex >> address >> std::dec >> length >> frames;
                Watch(mode, static_cast<u32>(address), length, frames, reply);
            } else if (command == "audio") {
                // audio on [seconds] | audio off | audio pos
                std::string mode;
                u32 seconds = Reader::AudioTap::DefaultSeconds;
                in >> mode >> seconds;
                if (mode == "on") {
                    Reader::AudioTap::Instance().SetEnabled(true, seconds);
                } else if (mode == "off") {
                    Reader::AudioTap::Instance().SetEnabled(false);
                }
                reply.body +=
                    std::format("audio position={}\n", Reader::AudioTap::Instance().Position());
            } else if (command == "wav") {
                // wav <start position> <end position, 0 = now> <path>
                u64 start = 0;
                u64 end = 0;
                in >> start >> end;
                WriteWav(rest(), start, end, reply);
            } else if (command == "save" || command == "load") {
                // save <path> | load <path>: a savestate
                SaveOrLoad(command == "save", rest(), reply);
            } else if (command == "status") {
                reply.body += std::format("frame={} game={} profile={} hooks={}{}\n",
                                          engine.Frame(), engine.GameId(),
                                          engine.HasProfile() ? "yes" : "no", engine.HookCount(),
                                          exited ? " exited" : "");
            } else if (command == "quit") {
                quit = true;
            } else {
                reply.Fail("unknown command " + command);
            }
        } catch (const std::exception& e) {
            reply.Fail(e.what());
        }

        std::scoped_lock lock{output_mutex};
        for (const auto& log : logs) {
            reply.body += log + "\n";
        }
        logs.clear();
        return reply;
    }

private:
    void Watch(const std::string& mode, u32 address, u32 length, u64 frames, Reply& reply) {
        const MemCheckCondition condition = mode == "r"    ? MEMCHECK_READ
                                            : mode == "w" ? MEMCHECK_WRITE
                                                          : MEMCHECK_READWRITE;
        g_breakpoints.AddMemCheck(address, address + length, condition, BREAK_ACTION_PAUSE);
        watching = true;
        watch_hits.clear();
        Run(frames, reply);
        watching = false;
        g_breakpoints.RemoveMemCheck(address, address + length);
        for (const auto& [pc, hit] : watch_hits) {
            reply.body += std::format("pc={:08x} ra={:08x} hits={}\n", pc, hit.first, hit.second);
        }
        reply.body += std::format("{} place(s)\n", watch_hits.size());
    }

    void Run(u64 frames, Reply& reply) {
        auto& engine = Reader::Engine::Instance();
        const u64 first = engine.Frame();
        const u64 target = first + frames;
        const auto started = Clock::now();
        while (engine.Frame() < target && !exited) {
            if (coreState == CORE_NEXTFRAME) {
                coreState = CORE_RUNNING_CPU;
            }
            if (watching && coreState == CORE_STEPPING_CPU) {
                // A watched access: note where it happened and go on past it.
                const u32 pc = currentMIPS->pc;
                auto& hit = watch_hits[pc];
                if (hit.second++ == 0) {
                    hit.first = currentMIPS->r[31];
                }
                g_breakpoints.SetSkipFirst(pc);
                coreState = CORE_RUNNING_CPU;
            }
            if (coreState != CORE_RUNNING_CPU) {
                exited = true;
                break;
            }
            PSP_RunLoopFor(static_cast<int>(usToCycles(1000000 / 60)));
        }
        if (coreState == CORE_NEXTFRAME) {
            coreState = CORE_RUNNING_CPU;
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        reply.body += std::format("frame={} ({} frames in {:.1f} s)\n", engine.Frame(),
                                  engine.Frame() - first, seconds);
        if (exited) {
            reply.Fail("the game stopped");
        }
    }

    void Screenshot(const std::filesystem::path& path, Reply& reply) {
        if (path.empty()) {
            reply.Fail("shot needs a path");
            return;
        }
        GPUDebugBuffer buffer;
        if (!gpuDebug || !gpuDebug->GetCurrentFramebuffer(buffer, GPU_DBG_FRAMEBUF_DISPLAY, 1)) {
            reply.Fail("the game has shown no picture yet");
            return;
        }
        u8* temp = nullptr;
        u32 width = ScreenWidth;
        u32 height = ScreenHeight;
        const u8* pixels = ConvertBufferToScreenshot(buffer, false, temp, width, height);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        const bool ok = pixels && Save888RGBScreenshot(Path(path.string()), ScreenshotFormat::PNG,
                                                       pixels, static_cast<int>(width),
                                                       static_cast<int>(height));
        delete[] temp;
        if (!ok) {
            reply.Fail("cannot write " + path.string());
        } else {
            reply.body += std::format("wrote {} at frame {}\n", path.string(),
                                      Reader::Engine::Instance().Frame());
        }
    }

    void LoadProfile(const std::filesystem::path& path, Reply& reply) {
        bool ok = false;
        const std::string source = ReadFile(path, ok);
        if (!ok) {
            reply.Fail("cannot read " + path.string());
            return;
        }
        {
            std::scoped_lock lock{output_mutex};
            profile_dir = path.parent_path();
        }
        auto& engine = Reader::Engine::Instance();
        if (!engine.LoadProfile(path.filename().string(), source)) {
            reply.Fail(engine.LastError());
        } else {
            reply.body += std::format("hooks={}\n", engine.HookCount());
        }
    }

    void HexDump(u64 address, std::size_t length, Reply& reply) {
        const std::string data = Reader::Engine::Instance().ReadMemory(address, length);
        for (std::size_t i = 0; i < data.size(); i += 16) {
            reply.body += std::format("{:08x}:", address + i);
            std::string ascii;
            for (std::size_t k = i; k < std::min(i + 16, data.size()); ++k) {
                const u8 byte = static_cast<u8>(data[k]);
                reply.body += std::format(" {:02x}", byte);
                ascii.push_back(byte >= 0x20 && byte < 0x7F ? static_cast<char>(byte) : '.');
            }
            reply.body += "  " + ascii + "\n";
        }
        if (data.size() < length) {
            reply.body += std::format("(unmapped after {} bytes)\n", data.size());
        }
    }

    void MemoryToFile(u64 address, std::size_t length, const std::filesystem::path& path,
                      Reply& reply) {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream file{path, std::ios::binary};
        if (!file) {
            reply.Fail("cannot write " + path.string());
            return;
        }
        const std::string data = Reader::Engine::Instance().ReadMemory(address, length);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        reply.body += std::format("wrote {} bytes\n", data.size());
    }

    void Find(const std::string& needle, u64 start, u64 end, Reply& reply) {
        if (needle.empty()) {
            reply.Fail("nothing to search for");
            return;
        }
        constexpr std::size_t MaxMatches = 64;
        auto& engine = Reader::Engine::Instance();
        std::size_t matches = 0;
        for (const auto& region : engine.MemoryRegions()) {
            const u64 low = std::max(region.base, start);
            const u64 high = std::min(region.base + region.size, end);
            if (low >= high || matches >= MaxMatches) {
                continue;
            }
            const std::string data = engine.ReadMemory(low, high - low);
            std::size_t at = 0;
            while ((at = data.find(needle, at)) != std::string::npos) {
                reply.body += std::format("{:08x}\n", low + at);
                if (++matches >= MaxMatches) {
                    reply.body += "(more matches not shown)\n";
                    break;
                }
                ++at;
            }
        }
        reply.body += std::format("{} match(es)\n", matches);
    }

    void FindPointers(u64 low, u64 high, Reply& reply) {
        constexpr std::size_t MaxMatches = 256;
        auto& engine = Reader::Engine::Instance();
        std::size_t matches = 0;
        for (const auto& region : engine.MemoryRegions()) {
            const std::string data = engine.ReadMemory(region.base, region.size);
            for (std::size_t at = 0; at + 4 <= data.size() && matches < MaxMatches; at += 4) {
                u32 value;
                std::memcpy(&value, data.data() + at, 4);
                // The PSP's addresses come with and without the top bits set.
                const u32 plain = value & 0x3FFFFFFF;
                if (plain >= low && plain < high) {
                    reply.body += std::format("{:08x}: {:08x}\n", region.base + at, value);
                    ++matches;
                }
            }
        }
        reply.body += std::format("{} match(es)\n", matches);
    }

    void WriteWav(const std::filesystem::path& path, u64 start, u64 end, Reply& reply) {
        auto& tap = Reader::AudioTap::Instance();
        if (end == 0) {
            end = tap.Position();
        }
        u64 actual = 0;
        const std::vector<s16> samples = tap.Read(start, end, actual);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream file{path, std::ios::binary};
        if (!file) {
            reply.Fail("cannot write " + path.string());
            return;
        }
        const u32 bytes = static_cast<u32>(samples.size() * 2);
        const auto put32 = [&file](u32 v) { file.write(reinterpret_cast<const char*>(&v), 4); };
        const auto put16 = [&file](u16 v) { file.write(reinterpret_cast<const char*>(&v), 2); };
        file.write("RIFF", 4);
        put32(36 + bytes);
        file.write("WAVEfmt ", 8);
        put32(16);
        put16(1);
        put16(2);
        put32(Reader::AudioTap::SampleRate);
        put32(Reader::AudioTap::SampleRate * 4);
        put16(4);
        put16(16);
        file.write("data", 4);
        put32(bytes);
        file.write(reinterpret_cast<const char*>(samples.data()), bytes);
        reply.body += std::format("wrote {} frames from position {}\n", samples.size() / 2, actual);
    }

    void SaveOrLoad(bool save, const std::filesystem::path& path, Reply& reply) {
        if (path.empty()) {
            reply.Fail("a path is needed");
            return;
        }
        if (save && path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        bool done = false;
        bool ok = false;
        std::string message;
        const auto callback = [&](SaveState::Status status, std::string_view text) {
            ok = status != SaveState::Status::FAILURE;
            message = std::string(text);
            done = true;
        };
        if (save) {
            SaveState::Save(Path(path.string()), -1, callback);
        } else {
            SaveState::Load(Path(path.string()), -1, callback);
        }
        SaveState::Process();
        if (!done || !ok) {
            reply.Fail(message.empty() ? "the savestate was not handled" : message);
        } else {
            reply.body += std::format("{} {}\n", save ? "saved" : "loaded", path.string());
        }
        if (coreState == CORE_NEXTFRAME || coreState == CORE_STEPPING_CPU) {
            coreState = CORE_RUNNING_CPU;
        }
    }

    bool booted = false;
    bool exited = false;
    bool watching = false;
    std::map<u32, std::pair<u32, u64>> watch_hits; ///< By pc: the first caller, the count
    std::mutex output_mutex;
    std::vector<std::string> events;
    std::vector<std::string> layouts;
    std::vector<std::string> logs;
    std::filesystem::path profile_dir;
};

bool WriteAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void ServeSocket(Harness& harness, const std::string& path) {
    ::unlink(path.c_str());
    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (server < 0 || path.size() >= sizeof(address.sun_path)) {
        std::cerr << "cannot create socket " << path << "\n";
        return;
    }
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::chmod(path.c_str(), 0600) != 0 || ::listen(server, 4) != 0) {
        std::cerr << "cannot listen on " << path << ": " << std::strerror(errno) << "\n";
        ::close(server);
        return;
    }
    std::cerr << "listening on " << path << std::endl;

    while (!harness.quit) {
        const int client = ::accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        // One request per connection: lines until the peer shuts down its write side.
        std::string request;
        char buffer[4096];
        ssize_t n;
        while ((n = ::read(client, buffer, sizeof(buffer))) > 0) {
            request.append(buffer, static_cast<std::size_t>(n));
        }
        std::istringstream lines{request};
        std::string line;
        std::string response;
        while (!harness.quit && std::getline(lines, line)) {
            response += harness.Execute(line).Format();
        }
        WriteAll(client, response);
        ::close(client);
    }
    ::close(server);
    ::unlink(path.c_str());
}

int Usage() {
    std::cerr << "usage: ppsspp-reader-harness --rom FILE --home DIR --assets DIR\n"
                 "           [--socket PATH] [--script FILE] [--profile FILE] [--interpreter]\n"
                 "Runs a PSP game without a display and takes commands on a socket (or, without\n"
                 "--socket, on standard input). See docs/reader/harness.md.\n";
    return 64;
}

} // Anonymous namespace

int main(int argc, char** argv) {
    PROFILE_INIT();
    TimeInit();
    std::signal(SIGPIPE, SIG_IGN);

    std::filesystem::path rom, home, assets, script, profile;
    std::string socket_path;
    bool interpreter = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--rom") {
            rom = value();
        } else if (arg == "--home") {
            home = value();
        } else if (arg == "--assets") {
            assets = value();
        } else if (arg == "--socket") {
            socket_path = value();
        } else if (arg == "--script") {
            script = value();
        } else if (arg == "--profile") {
            profile = value();
        } else if (arg == "--interpreter") {
            interpreter = true;
        } else {
            return Usage();
        }
    }
    if (rom.empty() || home.empty() || assets.empty()) {
        return Usage();
    }

    Harness harness;
    if (!harness.Boot(std::filesystem::absolute(rom), std::filesystem::absolute(home),
                      std::filesystem::absolute(assets), interpreter)) {
        return 1;
    }
    if (!profile.empty()) {
        std::cout << harness.Execute("profile " + profile.string()).Format();
    }
    if (!script.empty()) {
        std::ifstream file{script};
        std::string line;
        while (!harness.quit && std::getline(file, line)) {
            std::cout << harness.Execute(line).Format() << std::flush;
        }
    }
    if (!socket_path.empty()) {
        ServeSocket(harness, socket_path);
    } else if (script.empty()) {
        std::string line;
        while (!harness.quit && std::getline(std::cin, line)) {
            std::cout << harness.Execute(line).Format() << std::flush;
        }
    }
    harness.Shutdown();
    return 0;
}
