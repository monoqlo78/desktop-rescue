#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "core.h"

using Clock = std::chrono::steady_clock;

namespace {
constexpr DWORD PollMilliseconds = 2000;
constexpr DWORD IoMilliseconds = 1500;
constexpr size_t MaxRequest = 64;

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode desktop name.");
    std::string out(n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), out.data(), n, nullptr, nullptr))
        throw std::runtime_error("Could not encode desktop name.");
    return out;
}

std::string Quote(const std::string& value) { return "\"" + rescue::JsonEscape(value) + "\""; }
std::string Quote(const std::wstring& value) { return Quote(Utf8(value)); }
std::string Result(bool ok, const std::string& code, DWORD error = 0) {
    return "{\"ok\":" + std::string(ok ? "true" : "false") +
        ",\"code\":" + Quote(code) + ",\"win32Error\":" + std::to_string(error) + "}";
}

std::wstring ObjectName(HANDLE object) {
    DWORD needed = 0;
    GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &needed);
    if (!needed) return {};
    std::vector<wchar_t> text(needed / sizeof(wchar_t) + 1);
    if (!GetUserObjectInformationW(object, UOI_NAME, text.data(), needed, &needed)) return {};
    return text.data();
}

rescue::InputDesktop ReadInput() {
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desk) return {false, {}, GetLastError()};
    const auto name = ObjectName(desk);
    const auto error = name.empty() ? GetLastError() : ERROR_SUCCESS;
    CloseDesktop(desk);
    return {!name.empty(), name, error};
}

struct Identity {
    std::wstring logonSid;
    DWORD session = 0;
    std::wstring Endpoint() const {
        return L"DesktopRescue-" + std::to_wstring(session) + L"-" + logonSid;
    }
};

Identity CurrentIdentity() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        throw std::runtime_error("Cannot query process token.");
    Handle token(raw);
    DWORD bytes = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &bytes);
    std::vector<BYTE> data(bytes);
    if (!GetTokenInformation(token, TokenGroups, data.data(), bytes, &bytes))
        throw std::runtime_error("Cannot query token logon SID.");
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(data.data());
    Identity identity;
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            LPWSTR sid = nullptr;
            if (!ConvertSidToStringSidW(groups->Groups[i].Sid, &sid))
                throw std::runtime_error("Cannot encode logon SID.");
            identity.logonSid = sid;
            LocalFree(sid);
            break;
        }
    }
    if (identity.logonSid.empty() ||
        !GetTokenInformation(token, TokenSessionId, &identity.session, sizeof(DWORD), &bytes))
        throw std::runtime_error("An interactive logon session is required.");
    return identity;
}

class Security {
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
public:
    SECURITY_ATTRIBUTES attributes{};
    explicit Security(const Identity& id) {
        const auto sddl = L"D:P(A;;GA;;;" + id.logonSid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            throw std::runtime_error("Cannot create logon-session IPC security descriptor.");
        attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
    }
    ~Security() { LocalFree(descriptor_); }
};

struct TransferResult { bool ok; DWORD bytes; DWORD error; };

TransferResult FinishIo(HANDLE file, OVERLAPPED& ov, BOOL immediate, DWORD error,
                        HANDLE stop, DWORD timeout) {
    DWORD bytes = 0;
    if (!immediate && error != ERROR_IO_PENDING) return {false, 0, error};
    HANDLE waits[] = {ov.hEvent, stop};
    if (!immediate) {
        const auto wait = WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, timeout);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(file, &ov);
            GetOverlappedResult(file, &ov, &bytes, TRUE);
            return {false, 0, static_cast<DWORD>(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED)};
        }
    }
    if (!GetOverlappedResult(file, &ov, &bytes, FALSE)) return {false, 0, GetLastError()};
    return {true, bytes, 0};
}

TransferResult ReadOrWrite(HANDLE file, void* data, DWORD length, bool write, HANDLE stop) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return {false, 0, GetLastError()};
    OVERLAPPED ov{};
    ov.hEvent = event;
    DWORD bytes = 0;
    const auto immediate = write ? WriteFile(file, data, length, &bytes, &ov)
                                 : ReadFile(file, data, length, &bytes, &ov);
    return FinishIo(file, ov, immediate, immediate ? 0 : GetLastError(), stop, IoMilliseconds);
}

struct Reply { std::string text; bool stop = false; };

class PipeServer {
    Handle pipe_;
    HANDLE stop_;
    std::function<Reply(const std::string&)> dispatch_;
    std::thread thread_;
    std::atomic<DWORD> lastError_{0};
    void Loop() {
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event) { lastError_ = GetLastError(); SetEvent(stop_); break; }
            OVERLAPPED ov{};
            ov.hEvent = event;
            auto connected = ConnectNamedPipe(pipe_, &ov);
            auto error = connected ? 0 : GetLastError();
            TransferResult connectResult{true, 0, 0};
            if (!connected && error != ERROR_PIPE_CONNECTED)
                connectResult = FinishIo(pipe_, ov, FALSE, error, stop_, INFINITE);
            if (!connectResult.ok) {
                if (connectResult.error != ERROR_OPERATION_ABORTED) lastError_ = connectResult.error;
                break;
            }
            char request[MaxRequest]{};
            auto read = ReadOrWrite(pipe_, request, sizeof(request), false, stop_);
            if (read.ok && read.bytes > 0) {
                Reply reply;
                try {
                    reply = dispatch_(std::string(request, read.bytes));
                } catch (const std::exception& error) {
                    lastError_ = ERROR_GEN_FAILURE;
                    reply = {"{\"ok\":false,\"code\":\"request_failed\",\"message\":" + Quote(std::string(error.what())) + "}"};
                }
                auto write = ReadOrWrite(pipe_, const_cast<char*>(reply.text.data()),
                    static_cast<DWORD>(reply.text.size()), true, stop_);
                if (!write.ok) lastError_ = write.error;
                // A bounded acknowledgement avoids discarding an unread response on disconnect,
                // without FlushFileBuffers (which can block forever on an unresponsive client).
                if (write.ok) {
                    char acknowledgement[2]{};
                    auto ack = ReadOrWrite(pipe_, acknowledgement, sizeof(acknowledgement), false, stop_);
                    if (!ack.ok || ack.bytes != 2 || std::string(acknowledgement, 2) != "OK")
                        lastError_ = ack.ok ? ERROR_INVALID_DATA : ack.error;
                }
                if (reply.stop) SetEvent(stop_);
            } else if (read.error != ERROR_BROKEN_PIPE && read.error != ERROR_OPERATION_ABORTED) {
                lastError_ = read.error;
            }
            DisconnectNamedPipe(pipe_);
        }
    }
public:
    PipeServer(const std::wstring& endpoint, Security& security, HANDLE stop,
               std::function<Reply(const std::string&)> dispatch)
        : pipe_(CreateNamedPipeW((L"\\\\.\\pipe\\" + endpoint).c_str(),
              PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
              1, 65536, MaxRequest, 0, &security.attributes)),
          stop_(stop), dispatch_(std::move(dispatch)) {
        if (!pipe_) throw std::runtime_error("Cannot create private IPC pipe (Win32 " + std::to_string(GetLastError()) + ").");
    }
    void Start() { thread_ = std::thread([this] { Loop(); }); }
    ~PipeServer() { SetEvent(stop_); if (thread_.joinable()) thread_.join(); }
    DWORD Error() const { return lastError_.load(); }
};

struct ClientResult { std::string text; DWORD error; };

ClientResult Send(const std::wstring& endpoint, const std::string& command) {
    const auto name = L"\\\\.\\pipe\\" + endpoint;
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto deadline = Clock::now() + std::chrono::milliseconds(IoMilliseconds);
    while (true) {
        raw = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (raw != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY || Clock::now() >= deadline) return {{}, error};
        WaitNamedPipeW(name.c_str(), 100);
    }
    Handle pipe(raw);
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return {{}, GetLastError()};
    auto write = ReadOrWrite(pipe, const_cast<char*>(command.data()),
        static_cast<DWORD>(command.size()), true, nullptr);
    if (!write.ok) return {{}, write.error};
    std::vector<char> response(65536);
    auto read = ReadOrWrite(pipe, response.data(), static_cast<DWORD>(response.size()), false, nullptr);
    if (!read.ok) return {{}, read.error};
    char acknowledgement[] = "OK";
    const auto ack = ReadOrWrite(pipe, acknowledgement, 2, true, nullptr);
    if (!ack.ok) return {{}, ack.error};
    return {std::string(response.data(), read.bytes), 0};
}

std::atomic<HANDLE> wakeEvent{nullptr};
std::atomic<HANDLE> consoleStop{nullptr};

void CALLBACK DesktopChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (auto event = wakeEvent.load()) SetEvent(event);
}

BOOL WINAPI ConsoleControl(DWORD code) {
    if (code == CTRL_C_EVENT || code == CTRL_BREAK_EVENT || code == CTRL_CLOSE_EVENT) {
        if (auto stop = consoleStop.load()) { SetEvent(stop); return TRUE; }
    }
    return FALSE;
}

class DesktopHook {
    std::wstring name_;
    HANDLE recoveryEvent_;
    std::atomic<UINT>& requested_;
    UINT generation_;
    Handle ready_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle thread_;
    std::atomic<DWORD> error_{0};
    std::atomic<bool> active_{false};
    DWORD id_ = 0;
    static thread_local DesktopHook* current_;
    static LRESULT CALLBACK Keyboard(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION && current_) {
            const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (rescue::IsRecoveryKey(key->vkCode, key->flags,
                    message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
                current_->requested_.store(current_->generation_);
                if (!SetEvent(current_->recoveryEvent_))
                    current_->error_ = GetLastError();
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }
    static DWORD WINAPI Entry(void* self) { static_cast<DesktopHook*>(self)->Run(); return 0; }
    void Run() {
        current_ = this;
        const auto original = GetThreadDesktop(GetCurrentThreadId());
        const auto desktop = OpenDesktopW(name_.c_str(), 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_HOOKCONTROL);
        if (!desktop) { error_ = GetLastError(); SetEvent(ready_); return; }
        if (!SetThreadDesktop(desktop)) {
            error_ = GetLastError(); CloseDesktop(desktop); SetEvent(ready_); return;
        }
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        const auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, Keyboard, GetModuleHandleW(nullptr), 0);
        if (!hook) {
            error_ = GetLastError();
            SetThreadDesktop(original);
            CloseDesktop(desktop);
            SetEvent(ready_);
            return;
        }
        active_ = true;
        SetEvent(ready_);
        // Kernel events cross desktop boundaries without PostThreadMessage privilege requirements.
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            HANDLE waits[] = {stop_.value};
            const auto wait = MsgWaitForMultipleObjects(1, waits, FALSE, INFINITE, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) { error_ = GetLastError(); break; }
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        active_ = false;
        if (!UnhookWindowsHookEx(hook)) error_ = GetLastError();
        if (!SetThreadDesktop(original)) error_ = GetLastError();
        if (!CloseDesktop(desktop)) error_ = GetLastError();
        current_ = nullptr;
    }
public:
    DesktopHook(std::wstring name, HANDLE recoveryEvent, std::atomic<UINT>& requested, UINT generation)
        : name_(std::move(name)), recoveryEvent_(recoveryEvent), requested_(requested), generation_(generation) {
        if (!ready_ || !stop_) throw std::runtime_error("Cannot create hook wait events.");
        thread_.value = CreateThread(nullptr, 0, Entry, this, 0, &id_);
        if (!thread_) { error_ = GetLastError(); return; }
        WaitForSingleObject(ready_, INFINITE);
    }
    ~DesktopHook() {
        if (thread_) {
            if (WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT) {
                SetEvent(stop_);
                WaitForSingleObject(thread_, INFINITE);
            }
        }
    }
    bool Active() const { return active_.load(); }
    DWORD Error() const { return error_.load(); }
    const std::wstring& Name() const { return name_; }
};
thread_local DesktopHook* DesktopHook::current_ = nullptr;

unsigned long long CpuTicks() {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user))
        throw std::runtime_error("Cannot read process CPU time.");
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
}

std::string Resources(double elapsed, unsigned long long cpuStart) {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        throw std::runtime_error("Cannot read process memory.");
    unsigned threads = 0;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot) throw std::runtime_error("Cannot enumerate process threads.");
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot, &entry)) throw std::runtime_error("Cannot enumerate process threads.");
    do { if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++threads; } while (Thread32Next(snapshot, &entry));
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    DWORD handles = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles))
        throw std::runtime_error("Cannot read process handle count.");
    const auto cpu = (CpuTicks() - cpuStart) / 10000000.0 / (elapsed > 0 ? elapsed : 1) * 100;
    std::ostringstream out;
    out << std::fixed << std::setprecision(4)
        << "{\"sampleSeconds\":" << elapsed << ",\"cpuPercentOneCore\":" << cpu
        << ",\"cpuPercentMachine\":" << cpu / info.dwNumberOfProcessors
        << ",\"logicalProcessors\":" << info.dwNumberOfProcessors
        << ",\"workingSetBytes\":" << memory.WorkingSetSize
        << ",\"peakWorkingSetBytes\":" << memory.PeakWorkingSetSize
        << ",\"privateBytes\":" << memory.PrivateUsage
        << ",\"processCount\":1,\"threadCount\":" << threads
        << ",\"handleCount\":" << handles << "}";
    return out.str();
}

class Resident {
    HDESK baseline_ = nullptr;
    std::wstring baselineName_;
    Handle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle wake_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle recoverEvent_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic<UINT> requested_{0};
    std::unique_ptr<DesktopHook> hook_;
    UINT generation_ = 0;
    mutable std::mutex state_;
    std::mutex recovery_;
    std::wstring inputName_;
    std::wstring hookedName_;
    DWORD monitorError_ = 0;
    DWORD hookError_ = 0;
    bool hookActive_ = false;
    unsigned long long foreignObservations_ = 0;
    unsigned long long inaccessibleObservations_ = 0;
    bool observe_;
    rescue::RecoveryResult lastRecovery_{false, "not_attempted", {}, 0};
    double verificationMs_ = 0;
    const Clock::time_point started_ = Clock::now();
    const unsigned long long cpuStart_ = CpuTicks();
    void Refresh() {
        auto input = ReadInput();
        {
            std::lock_guard lock(state_);
            inputName_ = input.name;
            monitorError_ = input.error;
            if (!input.accessible) ++inaccessibleObservations_;
            else if (input.name != baselineName_) ++foreignObservations_;
        }
        if (observe_) return;
        if (!input.accessible || input.name == baselineName_ || rescue::IsSecureDesktop(input.name)) {
            hook_.reset();
        } else if (!hook_ || hook_->Name() != input.name || !hook_->Active()) {
            hook_.reset();
            hook_ = std::make_unique<DesktopHook>(input.name, recoverEvent_, requested_, ++generation_);
        }
        std::lock_guard lock(state_);
        hookedName_ = hook_ ? hook_->Name() : L"";
        hookActive_ = hook_ && hook_->Active();
        hookError_ = hook_ ? hook_->Error() : 0;
    }
    std::string Recover(const std::wstring& expected = {}) {
        if (observe_) return Result(false, "observe_only");
        std::lock_guard recoveryLock(recovery_);
        struct Ops {
            HDESK target;
            DWORD error = 0;
            rescue::InputDesktop Input() { return ReadInput(); }
            bool Switch() {
                const auto ok = SwitchDesktop(target);
                if (!ok) error = GetLastError();
                return ok != FALSE;
            }
            DWORD Error() const { return error; }
            void Wait() { Sleep(25); }
        } ops{baseline_};
        const auto begin = Clock::now();
        const auto result = rescue::Recover(ops, baselineName_, expected);
        {
            std::lock_guard lock(state_);
            lastRecovery_ = result;
            verificationMs_ = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        }
        SetEvent(wake_);
        return Result(result.ok, result.code, result.error);
    }
public:
    explicit Resident(bool observe) : observe_(observe) {
        if (!stop_ || !wake_ || !recoverEvent_) throw std::runtime_error("Cannot create resident wait events.");
        baseline_ = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | (observe ? 0 : DESKTOP_SWITCHDESKTOP));
        if (!baseline_) throw std::runtime_error("Input desktop is inaccessible; start on the normal desktop.");
        baselineName_ = ObjectName(baseline_);
        if (baselineName_.empty() || (!observe && baselineName_ != L"Default")) {
            CloseDesktop(baseline_); baseline_ = nullptr;
            throw std::runtime_error("Start refused: the input desktop must be Default. The recovery target is never learned from an alternate desktop.");
        }
    }
    ~Resident() {
        hook_.reset();
        if (baseline_) CloseDesktop(baseline_);
    }
    std::string Status(bool notifications, DWORD pipeError = 0) const {
        const auto input = ReadInput();
        std::lock_guard lock(state_);
        std::ostringstream out;
        out << "{\"ok\":true,\"code\":\"running\",\"pid\":" << GetCurrentProcessId()
            << ",\"observeOnly\":" << (observe_ ? "true" : "false")
            << ",\"baselineDesktop\":" << Quote(baselineName_)
            << ",\"inputDesktop\":" << Quote(input.name)
            << ",\"inputDesktopError\":" << input.error
            << ",\"hookDesktop\":" << Quote(hookedName_)
            << ",\"hookInstalled\":" << (hookActive_ ? "true" : "false")
            << ",\"hookError\":" << hookError_
            << ",\"monitorError\":" << monitorError_
            << ",\"foreignDesktopObservations\":" << foreignObservations_
            << ",\"inaccessibleDesktopObservations\":" << inaccessibleObservations_
            << ",\"pipeError\":" << pipeError
            << ",\"desktopSwitchNotifications\":" << (notifications ? "true" : "false")
            << ",\"fallbackPollMilliseconds\":" << PollMilliseconds
            << ",\"lastRecovery\":" << Quote(lastRecovery_.code)
            << ",\"lastRecoveryError\":" << lastRecovery_.error
            << ",\"recoverySourceDesktop\":" << Quote(lastRecovery_.before)
            << ",\"lastSwitchVerificationMilliseconds\":" << verificationMs_
            << ",\"resources\":" << Resources(std::chrono::duration<double>(Clock::now() - started_).count(), cpuStart_)
            << "}";
        return out.str();
    }
    int Run(const Identity& identity, unsigned probeSeconds) {
        Security security(identity);
        const auto endpoint = identity.Endpoint() + (observe_ ? L"-probe-" + std::to_wstring(GetCurrentProcessId()) : L"");
        Handle mutex(CreateMutexW(&security.attributes, FALSE, (L"Local\\" + endpoint).c_str()));
        const auto mutexError = GetLastError();
        if (!mutex) throw std::runtime_error("Cannot create resident mutex.");
        if (mutexError == ERROR_ALREADY_EXISTS) return 0;
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        wakeEvent = wake_.value;
        consoleStop = stop_.value;
        SetConsoleCtrlHandler(ConsoleControl, TRUE);
        const auto notification = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
            nullptr, DesktopChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
        {
            PipeServer pipe(endpoint, security, stop_, [&](const std::string& command) -> Reply {
                if (command == "status") return {Status(notification != nullptr)};
                if (command == "recover") return {Recover()};
                if (command == "stop") return {Result(true, "stopping"), true};
                return {Result(false, "invalid_command")};
            });
            pipe.Start();
            Refresh();
            auto nextPoll = Clock::now() + std::chrono::milliseconds(PollMilliseconds);
            const auto deadline = started_ + std::chrono::seconds(probeSeconds);
            while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
                auto now = Clock::now();
                if (observe_ && now >= deadline) break;
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextPoll - now).count();
                DWORD timeout = static_cast<DWORD>((remaining > 0) ? remaining : 0);
                if (observe_) {
                    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                    if (duration >= 0 && static_cast<unsigned long long>(duration) < timeout) timeout = static_cast<DWORD>(duration);
                }
                HANDLE handles[] = {stop_.value, wake_.value, recoverEvent_.value};
                const auto wait = MsgWaitForMultipleObjects(3, handles, FALSE, timeout, QS_ALLINPUT);
                if (wait == WAIT_FAILED) throw std::runtime_error("Resident event wait failed.");
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_OBJECT_0 + 2 && hook_ && hook_->Active() && requested_.load() == generation_)
                    Recover(hook_->Name());
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) {
                        SetEvent(stop_);
                    } else {
                        TranslateMessage(&message); DispatchMessageW(&message);
                    }
                }
                if (wait == WAIT_OBJECT_0 + 1 || Clock::now() >= nextPoll) {
                    Refresh();
                    nextPoll = Clock::now() + std::chrono::milliseconds(PollMilliseconds);
                }
            }
            if (observe_) std::cout << Status(notification != nullptr, pipe.Error()) << "\n";
            SetEvent(stop_);
        }
        hook_.reset();
        if (notification) UnhookWinEvent(notification);
        wakeEvent = nullptr; consoleStop = nullptr;
        SetConsoleCtrlHandler(ConsoleControl, FALSE);
        return 0;
    }
};

std::wstring Executable() {
    std::vector<wchar_t> path(32768);
    const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) throw std::runtime_error("Cannot determine executable path.");
    return std::wstring(path.data(), n);
}

int Start(const Identity& identity) {
    const auto existing = Send(identity.Endpoint(), "status");
    if (!existing.text.empty()) { std::cout << existing.text << "\n"; return 0; }
    if (existing.error != ERROR_FILE_NOT_FOUND && existing.error != ERROR_PIPE_BUSY)
        { std::cout << Result(false, "ipc_connect_failed", existing.error) << "\n"; return 1; }
    const auto input = ReadInput();
    if (!input.accessible || input.name != L"Default") {
        std::cout << Result(false, "start_requires_default_input_desktop", input.error) << "\n";
        return 1;
    }
    auto command = L"\"" + Executable() + L"\" resident";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(Executable().c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        std::cout << Result(false, "start_failed", GetLastError()) << "\n"; return 1;
    }
    Handle child(process.hProcess), childThread(process.hThread);
    for (int i = 0; i < 40; ++i) {
        const auto response = Send(identity.Endpoint(), "status");
        if (!response.text.empty()) { std::cout << response.text << "\n"; return 0; }
        DWORD exit = STILL_ACTIVE;
        GetExitCodeProcess(child, &exit);
        if (exit != STILL_ACTIVE) {
            std::cout << Result(false, "daemon_start_failed", exit) << "\n"; return 1;
        }
        Sleep(100);
    }
    // Signal only this newly-created process; no resident is killed by name.
    const auto response = Send(identity.Endpoint(), "stop");
    std::cout << Result(false, response.text.empty() ? "startup_timeout_check_status" : "startup_timeout_stopped") << "\n";
    return 1;
}

int AutoStart(const std::wstring& action) {
    constexpr auto subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto value = L"DesktopRescue";
    HKEY raw = nullptr;
    if (action == L"status") {
        auto error = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &raw);
        if (error == ERROR_FILE_NOT_FOUND) { std::cout << Result(true, "autostart_disabled") << "\n"; return 0; }
        if (error != ERROR_SUCCESS) { std::cout << Result(false, "registry_read_failed", error) << "\n"; return 1; }
        const auto query = RegQueryValueExW(raw, value, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(raw);
        std::cout << Result(query == ERROR_SUCCESS || query == ERROR_FILE_NOT_FOUND,
            query == ERROR_SUCCESS ? "autostart_enabled" : (query == ERROR_FILE_NOT_FOUND ?
                "autostart_disabled" : "registry_read_failed"), query == ERROR_FILE_NOT_FOUND ? 0 : query) << "\n";
        return query == ERROR_SUCCESS || query == ERROR_FILE_NOT_FOUND ? 0 : 1;
    }
    if (action != L"enable" && action != L"disable") {
        std::cout << Result(false, "invalid_autostart_action") << "\n"; return 2;
    }
    auto error = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &raw, nullptr);
    if (error != ERROR_SUCCESS) { std::cout << Result(false, "registry_write_failed", error) << "\n"; return 1; }
    if (action == L"enable") {
        const auto command = L"\"" + Executable() + L"\" start";
        error = RegSetValueExW(raw, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        error = RegDeleteValueW(raw, value);
        if (error == ERROR_FILE_NOT_FOUND) error = ERROR_SUCCESS;
    }
    RegCloseKey(raw);
    std::cout << Result(error == ERROR_SUCCESS, error == ERROR_SUCCESS ?
        (action == L"enable" ? "autostart_enabled" : "autostart_disabled") : "registry_write_failed", error) << "\n";
    return error == ERROR_SUCCESS ? 0 : 1;
}

int SelfTest() {
    int count = 0;
    const auto check = [&](bool condition, const char* name) {
        if (!condition) throw std::runtime_error(std::string("Test failed: ") + name);
        ++count;
    };
    check(rescue::IsRecoveryKey(VK_HOME, 0, true), "physical Home");
    check(rescue::IsRecoveryKey(VK_ESCAPE, 0, true), "physical Esc");
    check(!rescue::IsRecoveryKey(VK_HOME, LLKHF_INJECTED, true), "ignore injected");
    check(!rescue::IsRecoveryKey(VK_HOME, LLKHF_LOWER_IL_INJECTED, true), "ignore lower-integrity injected");
    check(!rescue::IsRecoveryKey(VK_HOME, 0, false), "ignore key-up");
    check(!rescue::IsRecoveryKey('A', 0, true), "ignore normal keys");
    check(rescue::JsonEscape("\"\\\n\x01") == "\\\"\\\\\\n\\u0001", "JSON escaping");
    struct Fake {
        std::vector<rescue::InputDesktop> inputs;
        size_t index = 0;
        bool switchOk = true;
        int switches = 0;
        int waits = 0;
        rescue::InputDesktop Input() { return inputs[(index < inputs.size()) ? index++ : inputs.size()-1]; }
        bool Switch() { ++switches; return switchOk; }
        DWORD Error() const { return ERROR_ACCESS_DENIED; }
        void Wait() { ++waits; }
    };
    Fake normal{{{true, L"Default", 0}}};
    check(rescue::Recover(normal, L"Default").ok && normal.switches == 0, "normal desktop is untouched");
    Fake denied{{{false, {}, ERROR_ACCESS_DENIED}}};
    check(!rescue::Recover(denied, L"Default").ok && denied.switches == 0, "inaccessible input");
    Fake secure{{{true, L"Winlogon", 0}}};
    check(!rescue::Recover(secure, L"Default").ok && secure.switches == 0, "secure desktop excluded");
    Fake race{{{true, L"Other", 0}}};
    check(rescue::Recover(race, L"Default", L"Foreign").code == "input_changed" && race.switches == 0, "key/switch race");
    Fake failure{{{true, L"Foreign", 0}}, 0, false};
    check(rescue::Recover(failure, L"Default").code == "switch_failed", "switch failure surfaced");
    Fake falseSuccess{{{true, L"Foreign", 0}}};
    check(rescue::Recover(falseSuccess, L"Default").code == "verification_failed" && falseSuccess.waits == 20, "switch success is not sufficient");
    Fake success{{{true, L"Foreign", 0}, {true, L"Default", 0}}};
    check(rescue::Recover(success, L"Default").code == "recovered_verified" && success.switches == 1, "verify active desktop");

    const auto identity = CurrentIdentity();
    Security security(identity);
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    check(static_cast<bool>(stop), "IPC stop event");
    auto endpoint = identity.Endpoint() + L"-test-" + std::to_wstring(GetCurrentProcessId());
    {
        PipeServer pipe(endpoint, security, stop, [](const std::string& command) -> Reply {
            if (command == "status") return {Result(true, "test_status")};
            if (command == "stop") return {Result(true, "stopping"), true};
            return {Result(false, "invalid_command")};
        });
        pipe.Start();
        bool duplicateRejected = false;
        try {
            PipeServer duplicate(endpoint, security, stop, [](const std::string&) -> Reply { return {}; });
        } catch (const std::runtime_error&) { duplicateRejected = true; }
        check(duplicateRejected, "IPC endpoint cannot be replaced");
        check(Send(endpoint, "status").text == Result(true, "test_status"), "logon-SID IPC request");
        Sleep(20);
        check(Send(endpoint, "garbage").text == Result(false, "invalid_command"), "IPC rejects commands");
        Sleep(20);
        check(Send(endpoint, std::string(128, 'x')).text.empty(), "IPC request size bound");
        Sleep(20);
        check(Send(endpoint, "status").text == Result(true, "test_status"), "IPC survives malformed input");
        Sleep(20);
        {
            Handle stalled(CreateFileW((L"\\\\.\\pipe\\" + endpoint).c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr));
            check(static_cast<bool>(stalled), "IPC stalled client connection");
            Sleep(IoMilliseconds + 100);
        }
        check(Send(endpoint, "status").text == Result(true, "test_status"), "IPC read deadline releases stalled client");
        Sleep(20);
        check(Send(endpoint, "stop").text == Result(true, "stopping"), "IPC clean stop response");
    }
    std::cout << "{\"ok\":true,\"testsPassed\":" << count
        << ",\"realDesktopSwitches\":0,\"keyboardHooksInstalled\":0,\"startupChanges\":0}\n";
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"self-test") return SelfTest();
        if (argc == 2 && std::wstring(argv[1]) == L"version") {
            std::cout << "desktop-rescue 0.1.0\n"; return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"autostart") return AutoStart(argv[2]);
        if (argc == 4 && std::wstring(argv[1]) == L"probe" && std::wstring(argv[2]) == L"--seconds") {
            size_t used = 0;
            const std::wstring text(argv[3]);
            const auto seconds = std::stoul(text, &used);
            if (used != text.size() || seconds < 1 || seconds > 3600)
                throw std::runtime_error("probe duration must be 1..3600 seconds.");
            Resident resident(true);
            return resident.Run(CurrentIdentity(), static_cast<unsigned>(seconds));
        }
        if (argc == 2) {
            const std::wstring command(argv[1]);
            const auto identity = CurrentIdentity();
            if (command == L"resident") { Resident resident(false); return resident.Run(identity, 0); }
            if (command == L"start") return Start(identity);
            if (command == L"stop" || command == L"status" || command == L"recover") {
                const auto response = Send(identity.Endpoint(), Utf8(command));
                if (response.text.empty()) {
                    std::cout << Result(false, response.error == ERROR_FILE_NOT_FOUND ?
                        "not_running" : "ipc_failed", response.error) << "\n";
                    return 1;
                }
                std::cout << response.text << "\n";
                return response.text.find("\"ok\":true") != std::string::npos ? 0 : 1;
            }
        }
        std::cerr << "Usage: desktop-rescue start|stop|status|recover|version|self-test\n"
                  << "       desktop-rescue probe --seconds N\n"
                  << "       desktop-rescue autostart status|enable|disable\n";
        return 2;
    } catch (const std::exception& error) {
        std::cout << "{\"ok\":false,\"code\":\"error\",\"message\":" << Quote(std::string(error.what())) << "}\n";
        return 1;
    }
}
