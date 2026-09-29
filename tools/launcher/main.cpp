// PimaxGaze.exe: registers the gaze layer for the current user and runs the
// gaze service in this console. It does not touch BrokenEye; start that
// yourself (administrator, Connect, preview on).
//
// Lives next to XR_APILAYER_PIMAXGAZE_eye_gaze.json (dist/layer/). Finds the
// repository by walking up from its own folder to service\gaze_service.py.
//
// usage:
//   PimaxGaze.exe [launcher options] [service options]
// launcher options:
//   --python <exe>    Python interpreter (default: python.exe on PATH, else py.exe -3)
//   --repo <dir>      repository root (default: search upward from the exe)
//   --manifest <json> layer manifest to register (default: next to the exe)
//   --no-register     start the service without touching the registry
//   --register-only   register the layer and exit
//   --unregister      remove every PimaxGaze layer registration and exit
// Everything else is passed to gaze_service.py. Without --calib the launcher
// adds --calib <repo>\service\gaze_calib.json.

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

const wchar_t* kImplicitKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
const wchar_t* kManifestName = L"XR_APILAYER_PIMAXGAZE_eye_gaze.json";
const wchar_t* kShmName = L"Local\\PimaxGazeLayer";
constexpr int64_t kMaxHeartbeatAgeUs = 1'500'000; // same threshold as the layer

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring full_path(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return (n && n < std::size(buf)) ? std::wstring(buf, n) : p;
}

std::wstring parent_dir(const std::wstring& p) {
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return parent_dir(std::wstring(buf, n));
}

bool ends_with_nocase(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           _wcsicmp(s.c_str() + (s.size() - suffix.size()), suffix.c_str()) == 0;
}

// Quotes one argument for CreateProcess / CommandLineToArgvW.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

// ------------------------------------------------------------------ registry

std::vector<std::wstring> registered_manifests() {
    std::vector<std::wstring> out;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kImplicitKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[MAX_PATH * 4];
        DWORD len = static_cast<DWORD>(std::size(name));
        const LONG r = RegEnumValueW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_SUCCESS && ends_with_nocase(name, kManifestName)) out.emplace_back(name, len);
    }
    RegCloseKey(key);
    return out;
}

// Removes every PimaxGaze registration except `keep` (pass empty to remove all).
bool remove_registrations(const std::wstring& keep) {
    const auto names = registered_manifests();
    if (names.empty()) return true;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kImplicitKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    bool ok = true;
    for (const auto& n : names) {
        if (!keep.empty() && _wcsicmp(n.c_str(), keep.c_str()) == 0) continue;
        if (RegDeleteValueW(key, n.c_str()) == ERROR_SUCCESS) std::wprintf(L"unregistered: %ls\n", n.c_str());
        else { std::wprintf(L"could not unregister: %ls\n", n.c_str()); ok = false; }
    }
    RegCloseKey(key);
    return ok;
}

bool register_layer(const std::wstring& manifest) {
    remove_registrations(manifest); // stale copies from other folders would load a second layer
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kImplicitKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        std::wprintf(L"error: cannot open HKCU\\%ls\n", kImplicitKey);
        return false;
    }
    const DWORD enabled = 0; // 0 = enabled for the OpenXR loader
    const LONG r = RegSetValueExW(key, manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof enabled);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        std::wprintf(L"error: cannot register %ls\n", manifest.c_str());
        return false;
    }
    std::wprintf(L"layer registered: %ls\n", manifest.c_str());
    return true;
}

// ------------------------------------------------------------------ service

int64_t qpc_us() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart));
}

// True when a service instance has written its heartbeat recently.
bool service_alive() {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
    if (!map) return false;
    bool alive = false;
    if (const auto* view = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 88))) {
        int64_t heartbeat;
        std::memcpy(&heartbeat, view + 80, sizeof heartbeat);
        alive = std::memcmp(view, "PGZL", 4) == 0 && heartbeat != 0 && qpc_us() - heartbeat < kMaxHeartbeatAgeUs;
        UnmapViewOfFile(view);
    }
    CloseHandle(map);
    return alive;
}

std::wstring find_repo(std::wstring dir) {
    while (!dir.empty()) {
        if (file_exists(dir + L"\\service\\gaze_service.py")) return dir;
        const std::wstring up = parent_dir(dir);
        if (up == dir) break;
        dir = up;
    }
    return {};
}

// Returns the interpreter command line prefix, e.g. `"C:\...\python.exe"` or `"C:\...\py.exe" -3`.
std::wstring find_python() {
    wchar_t buf[MAX_PATH * 4];
    if (SearchPathW(nullptr, L"python.exe", nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr)) return quote(buf);
    if (SearchPathW(nullptr, L"py.exe", nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr)) return quote(buf) + L" -3";
    return {};
}

int run_service(const std::wstring& python, const std::wstring& repo, const std::vector<std::wstring>& service_args) {
    std::wstring cmd = python + L" -u " + quote(repo + L"\\service\\gaze_service.py");
    for (const auto& a : service_args) cmd += L" " + quote(a);
    std::wprintf(L"starting: %ls\n\n", cmd.c_str());

    // Kill the service with the launcher (window closed, launcher killed).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
    }

    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        repo.c_str(), &si, &pi)) {
        std::wprintf(L"error: cannot start Python (error %lu). Pass --python <path to python.exe>.\n", GetLastError());
        return 1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Ctrl+C reaches the service (same console); the launcher waits for it to exit.
    SetConsoleCtrlHandler(nullptr, TRUE);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    std::wprintf(L"\nservice exited (code %lu)\n", code);
    return static_cast<int>(code);
}

void pause_if_own_console() {
    // Double-clicked: the console closes with us, so keep errors readable.
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) <= 1) {
        std::wprintf(L"\npress Enter to close\n");
        std::getwchar();
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring python, repo, manifest;
    bool do_register = true, register_only = false, unregister = false, have_calib = false;
    std::vector<std::wstring> service_args;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](std::wstring& out) {
            if (i + 1 >= argc) { std::wprintf(L"error: %ls needs a value\n", a.c_str()); std::exit(2); }
            out = argv[++i];
        };
        if (a == L"--python") value(python);
        else if (a == L"--repo") value(repo);
        else if (a == L"--manifest") value(manifest);
        else if (a == L"--no-register") do_register = false;
        else if (a == L"--register-only") register_only = true;
        else if (a == L"--unregister") unregister = true;
        else if (a == L"-h" || a == L"--help") {
            std::wprintf(L"PimaxGaze.exe [--python exe] [--repo dir] [--manifest json] [--no-register | --register-only | --unregister] [gaze_service.py options]\n");
            return 0;
        } else {
            if (a == L"--calib" || a.rfind(L"--calib=", 0) == 0) have_calib = true;
            service_args.push_back(a);
        }
    }

    if (unregister) return remove_registrations(L"") ? 0 : 1;

    const std::wstring here = exe_dir();
    int rc = 0;
    if (do_register || register_only) {
        manifest = full_path(manifest.empty() ? here + L"\\" + kManifestName : manifest);
        if (!file_exists(manifest)) {
            std::wprintf(L"error: layer manifest not found: %ls\n", manifest.c_str());
            pause_if_own_console();
            return 1;
        }
        if (!file_exists(parent_dir(manifest) + L"\\PimaxGazeLayer.dll"))
            std::wprintf(L"warning: PimaxGazeLayer.dll is not next to the manifest\n");
        if (!register_layer(manifest)) { pause_if_own_console(); return 1; }
    }
    if (register_only) return 0;

    if (service_alive()) {
        std::wprintf(L"the gaze service is already running (fresh heartbeat); not starting a second one\n");
        pause_if_own_console();
        return 0;
    }

    repo = repo.empty() ? find_repo(here) : full_path(repo);
    if (repo.empty() || !file_exists(repo + L"\\service\\gaze_service.py")) {
        std::wprintf(L"error: service\\gaze_service.py not found above %ls; pass --repo <dir>\n", here.c_str());
        pause_if_own_console();
        return 1;
    }
    if (!have_calib) {
        const std::wstring calib = repo + L"\\service\\gaze_calib.json";
        if (!file_exists(calib)) {
            std::wprintf(L"error: no calibration at %ls; pass --calib <file>\n", calib.c_str());
            pause_if_own_console();
            return 1;
        }
        service_args.insert(service_args.begin(), {L"--calib", calib});
    }
    if (python.empty()) python = find_python();
    else python = quote(python);
    if (python.empty()) {
        std::wprintf(L"error: python.exe / py.exe not on PATH; pass --python <path>\n");
        pause_if_own_console();
        return 1;
    }

    rc = run_service(python, repo, service_args);
    if (rc != 0) pause_if_own_console();
    return rc;
}
