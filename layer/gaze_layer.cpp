// Pimax gaze layer: an OpenXR implicit API layer that replaces the poses of
// XR_EXT_eye_gaze_interaction with the camera-based estimate published by
// service/gaze_service.py in shared memory.
//
// The runtime underneath (Pimax OpenXR or SteamVR) already exposes the
// extension, so nothing is added or hidden: every call goes down unchanged,
// and only the results for the gaze action are rewritten.
//   - xrSuggestInteractionProfileBindings: remember which actions the app
//     bound to /user/eyes_ext/input/gaze_ext/pose.
//   - xrCreateActionSpace: remember the spaces created from those actions.
//   - xrLocateSpace / xrLocateSpaces: for a gaze space, take the head pose
//     from a VIEW reference space the layer owns, compose the service's gaze
//     direction onto it, and report it tracked. When the service is absent,
//     stale or not confident, keep the runtime's pose but clear the tracked
//     bits, so consumers fall back to fixed foveation.
//   - xrGetActionStatePose: isActive while the service is alive.
// Fail open: nothing here blocks, throws across the boundary, or returns an
// error the runtime did not return.
//
// Environment: PIMAXGAZE_OVERRIDE=0 keeps the layer passive (logging only).
// Log file: %LOCALAPPDATA%\PimaxGazeLayer\layer-<pid>.log

#define XR_NO_PROTOTYPES
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr char kLayerName[] = "XR_APILAYER_PIMAXGAZE_eye_gaze";
constexpr XrVersion kLayerApiVersion = XR_MAKE_VERSION(1, 0, 0);
constexpr wchar_t kShmName[] = L"Local\\PimaxGazeLayer";
constexpr int64_t kMaxSampleAgeUs = 200'000;      // newest camera frame older than this: untracked
constexpr int64_t kMaxHeartbeatAgeUs = 1'500'000; // service silent longer than this: dead
constexpr float kMinConfidence = 0.5f;
constexpr int64_t kHoldUs = 250'000;               // last good gaze bridges dropouts this long

std::atomic<float> g_last_good_yaw{0.f}, g_last_good_pitch{0.f};
std::atomic<int64_t> g_last_good_us{INT64_MIN / 2};

// ------------------------------------------------------------------ logging

FILE* g_log = nullptr;
std::mutex g_log_mutex;

void log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) {
        char dir[MAX_PATH];
        const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        std::string path = std::string(dir) + "\\PimaxGazeLayer";
        CreateDirectoryA(path.c_str(), nullptr);
        path += "\\layer-" + std::to_string(GetCurrentProcessId()) + ".log";
        g_log = std::fopen(path.c_str(), "a");
        if (!g_log) return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

// ------------------------------------------------------------------ clock

int64_t qpc_us() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart));
}

// ------------------------------------------------------------------ shared memory

#pragma pack(push, 1)
struct GazeBlock {
    char magic[4];
    uint32_t version, seq, reserved;
    int64_t sample_qpc_us;
    float yaw, pitch;
    float dir[3];
    float confidence;
    uint32_t flags, reserved2;
    float left[3], right[3];
    int64_t heartbeat_qpc_us;
};
#pragma pack(pop)
static_assert(sizeof(GazeBlock) == 88, "shared block layout must match gaze_service.py");

class SharedReader {
public:
    // Returns true when a consistent block was read.
    bool read(GazeBlock& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!view_) {
            const int64_t now = qpc_us();
            if (now - last_attempt_us_ < 1'000'000) return false;
            last_attempt_us_ = now;
            map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kShmName);
            if (!map_) return false;
            view_ = static_cast<const volatile GazeBlock*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, sizeof(GazeBlock)));
            if (!view_) { CloseHandle(map_); map_ = nullptr; return false; }
            log("shared memory opened");
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint32_t s1 = view_->seq;
            if (s1 & 1u) continue;
            GazeBlock copy;
            std::memcpy(&copy, const_cast<const GazeBlock*>(view_), sizeof copy);
            MemoryBarrier();
            const uint32_t s2 = view_->seq;
            if (s1 == s2 && std::memcmp(copy.magic, "PGZL", 4) == 0) { out = copy; return true; }
        }
        return false;
    }

private:
    std::mutex mutex_;
    HANDLE map_ = nullptr;
    const volatile GazeBlock* view_ = nullptr;
    int64_t last_attempt_us_ = -10'000'000;
};

SharedReader g_shm;

// ------------------------------------------------------------------ quaternion helpers

XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrVector3f qrot(const XrQuaternionf& q, const XrVector3f& v) {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const float tx = 2.f * (q.y * v.z - q.z * v.y), ty = 2.f * (q.z * v.x - q.x * v.z), tz = 2.f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

// Orientation whose -Z axis points along the gaze direction given as yaw
// (+ right) and pitch (+ up) in degrees, in the head frame: q = Ry(-yaw) * Rx(pitch).
XrQuaternionf gaze_quat(float yaw_deg, float pitch_deg) {
    const float hy = -yaw_deg * 3.14159265f / 360.f, hp = pitch_deg * 3.14159265f / 360.f;
    const XrQuaternionf qy{0.f, std::sin(hy), 0.f, std::cos(hy)};
    const XrQuaternionf qx{std::sin(hp), 0.f, 0.f, std::cos(hp)};
    return qmul(qy, qx);
}

// ------------------------------------------------------------------ state

struct Dispatch {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr = nullptr;
    PFN_xrDestroyInstance destroy_instance = nullptr;
    PFN_xrCreateSession create_session = nullptr;
    PFN_xrDestroySession destroy_session = nullptr;
    PFN_xrCreateReferenceSpace create_reference_space = nullptr;
    PFN_xrCreateActionSpace create_action_space = nullptr;
    PFN_xrDestroySpace destroy_space = nullptr;
    PFN_xrLocateSpace locate_space = nullptr;
    PFN_xrLocateSpaces locate_spaces = nullptr;   // OpenXR 1.1, may be null
    PFN_xrSuggestInteractionProfileBindings suggest_bindings = nullptr;
    PFN_xrGetActionStatePose get_action_state_pose = nullptr;
    PFN_xrPathToString path_to_string = nullptr;
};

struct GazeSpace {
    XrSession session;
    XrPosef offset;   // poseInActionSpace
    XrAction action;
};

struct SessionState {
    XrInstance instance;
    XrSpace view_space = XR_NULL_HANDLE;
    bool view_space_failed = false;
};

std::mutex g_mutex;
std::unordered_map<XrInstance, Dispatch> g_instances;
std::unordered_set<XrAction> g_gaze_actions;
std::unordered_map<XrSpace, GazeSpace> g_gaze_spaces;
// Action spaces whose action is not (yet) bound to the gaze pose. OpenXR lets an
// app create an action space before suggesting bindings - Cheeky's layer creates
// its gaze space in xrCreateSession and suggests the binding at attach time - so
// a space is promoted to g_gaze_spaces when its action's binding arrives.
std::unordered_map<XrSpace, GazeSpace> g_pending_spaces;
std::unordered_map<XrSession, SessionState> g_sessions;
bool g_override = true;

struct Counters {
    std::atomic<uint64_t> locates{0}, overridden{0}, held{0}, untracked{0}, no_service{0}, passthrough{0};
    std::atomic<int64_t> last_report_us{0};
} g_counters;

void maybe_report() {
    const int64_t now = qpc_us();
    int64_t last = g_counters.last_report_us.load();
    if (now - last < 5'000'000) return;
    if (!g_counters.last_report_us.compare_exchange_strong(last, now)) return;
    GazeBlock b{};
    const bool have = g_shm.read(b);
    log("locates %llu: overridden %llu, held %llu, untracked %llu, no-service %llu, passthrough %llu | service %s conf %.2f flags %u age %lld ms",
        static_cast<unsigned long long>(g_counters.locates.load()), static_cast<unsigned long long>(g_counters.overridden.load()),
        static_cast<unsigned long long>(g_counters.held.load()), static_cast<unsigned long long>(g_counters.untracked.load()), static_cast<unsigned long long>(g_counters.no_service.load()),
        static_cast<unsigned long long>(g_counters.passthrough.load()), have ? "up" : "absent",
        have ? b.confidence : 0.f, have ? b.flags : 0u, have ? static_cast<long long>((now - b.sample_qpc_us) / 1000) : -1LL);
}

const Dispatch* dispatch_for_instance(XrInstance instance) {
    const auto it = g_instances.find(instance);
    return it == g_instances.end() ? nullptr : &it->second;
}

// The session's private VIEW space, created on first use.
XrSpace view_space_for(SessionState& s, const Dispatch& d, XrSession session) {
    if (s.view_space != XR_NULL_HANDLE || s.view_space_failed) return s.view_space;
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    ci.poseInReferenceSpace.orientation.w = 1.f;
    const XrResult r = d.create_reference_space(session, &ci, &s.view_space);
    if (XR_FAILED(r)) {
        s.view_space_failed = true;
        s.view_space = XR_NULL_HANDLE;
        log("creating the VIEW space failed (%d); gaze cannot be overridden for this session", static_cast<int>(r));
    }
    return s.view_space;
}

// Rewrites `loc` (already filled by the runtime) for a gaze space.
void apply_gaze(const Dispatch& d, const GazeSpace& gs, XrSpace base, XrTime time, XrSpaceLocation& loc) {
    g_counters.locates++;
    maybe_report();
    if (!g_override) { g_counters.passthrough++; return; }

    // Consumers such as Cheeky use any location with ORIENTATION_VALID set, so
    // the runtime's own pose must never show through with it: that alternated
    // the foveation between the real gaze and the runtime's placeholder on
    // every dropped camera frame. The last good gaze is held, valid but not
    // tracked, across short dropouts; after that the location is invalid.
    GazeBlock b{};
    const int64_t now = qpc_us();
    const bool have = g_shm.read(b);
    const bool alive = have && (now - b.heartbeat_qpc_us) < kMaxHeartbeatAgeUs;
    const bool fresh = alive && (now - b.sample_qpc_us) < kMaxSampleAgeUs && b.confidence >= kMinConfidence;
    float yaw = b.yaw, pitch = b.pitch;
    bool held = false;
    if (fresh) {
        g_last_good_yaw.store(b.yaw);
        g_last_good_pitch.store(b.pitch);
        g_last_good_us.store(now);
    } else if (alive && now - g_last_good_us.load() < kHoldUs) {
        yaw = g_last_good_yaw.load();
        pitch = g_last_good_pitch.load();
        held = true;
    } else {
        (alive ? g_counters.untracked : g_counters.no_service)++;
        loc.locationFlags = 0;
        return;
    }

    XrSpace view = XR_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_sessions.find(gs.session);
        if (it != g_sessions.end()) view = view_space_for(it->second, d, gs.session);
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (view == XR_NULL_HANDLE || XR_FAILED(d.locate_space(view, base, time, &head)) ||
        !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        g_counters.untracked++;
        loc.locationFlags = 0;
        return;
    }
    const XrQuaternionf gaze = qmul(qmul(head.pose.orientation, gaze_quat(yaw, pitch)), gs.offset.orientation);
    const XrVector3f off = qrot(head.pose.orientation, gs.offset.position);
    loc.pose.orientation = gaze;
    loc.pose.position = {head.pose.position.x + off.x, head.pose.position.y + off.y, head.pose.position.z + off.z};
    loc.locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | (held ? 0 : XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) |
                        (head.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT));
    (held ? g_counters.held : g_counters.overridden)++;
}

// ------------------------------------------------------------------ intercepted entry points

XRAPI_ATTR XrResult XRAPI_CALL layer_locate_space(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
    const Dispatch* d = nullptr;
    GazeSpace gs{};
    bool is_gaze = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_gaze_spaces.find(space);
        if (it != g_gaze_spaces.end()) {
            is_gaze = true;
            gs = it->second;
            auto s = g_sessions.find(gs.session);
            if (s != g_sessions.end()) d = dispatch_for_instance(s->second.instance);
        } else if (!g_instances.empty()) {
            d = &g_instances.begin()->second;
        }
    }
    if (!d) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = d->locate_space(space, base, time, location);
    if (XR_SUCCEEDED(r) && is_gaze && location) apply_gaze(*d, gs, base, time, *location);
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_locate_spaces(XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* locations) {
    const Dispatch* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto s = g_sessions.find(session);
        if (s != g_sessions.end()) d = dispatch_for_instance(s->second.instance);
        else if (!g_instances.empty()) d = &g_instances.begin()->second;
    }
    if (!d || !d->locate_spaces) return XR_ERROR_FUNCTION_UNSUPPORTED;
    const XrResult r = d->locate_spaces(session, info, locations);
    if (XR_FAILED(r) || !info || !locations || !locations->locations) return r;
    for (uint32_t i = 0; i < info->spaceCount && i < locations->locationCount; ++i) {
        GazeSpace gs{};
        bool is_gaze = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_gaze_spaces.find(info->spaces[i]);
            if (it != g_gaze_spaces.end()) { is_gaze = true; gs = it->second; }
        }
        if (!is_gaze) continue;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        loc.locationFlags = locations->locations[i].locationFlags;
        loc.pose = locations->locations[i].pose;
        apply_gaze(*d, gs, info->baseSpace, info->time, loc);
        locations->locations[i].locationFlags = loc.locationFlags;
        locations->locations[i].pose = loc.pose;
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_action_state_pose(XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* state) {
    const Dispatch* d = nullptr;
    bool is_gaze = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto s = g_sessions.find(session);
        if (s != g_sessions.end()) d = dispatch_for_instance(s->second.instance);
        else if (!g_instances.empty()) d = &g_instances.begin()->second;
        if (info) is_gaze = g_gaze_actions.count(info->action) != 0;
    }
    if (!d) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = d->get_action_state_pose(session, info, state);
    if (XR_SUCCEEDED(r) && is_gaze && state && g_override) {
        GazeBlock b{};
        if (g_shm.read(b) && (qpc_us() - b.heartbeat_qpc_us) < kMaxHeartbeatAgeUs) state->isActive = XR_TRUE;
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_suggest_bindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* sb) {
    const Dispatch* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        d = dispatch_for_instance(instance);
    }
    if (!d) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = d->suggest_bindings(instance, sb);
    if (!sb) return r;
    char profile[XR_MAX_PATH_LENGTH] = "";
    uint32_t n = 0;
    if (XR_FAILED(d->path_to_string(instance, sb->interactionProfile, sizeof profile, &n, profile))) return r;
    if (std::strcmp(profile, "/interaction_profiles/ext/eye_gaze_interaction") != 0) return r;
    int recorded = 0;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; ++i) {
        char binding[XR_MAX_PATH_LENGTH] = "";
        if (XR_FAILED(d->path_to_string(instance, sb->suggestedBindings[i].binding, sizeof binding, &n, binding))) continue;
        if (std::strcmp(binding, "/user/eyes_ext/input/gaze_ext/pose") == 0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            const XrAction action = sb->suggestedBindings[i].action;
            g_gaze_actions.insert(action);
            ++recorded;
            for (auto it = g_pending_spaces.begin(); it != g_pending_spaces.end();) {
                if (it->second.action != action) { ++it; continue; }
                g_gaze_spaces[it->first] = it->second;
                log("gaze action space promoted at binding (%zu tracked)", g_gaze_spaces.size());
                it = g_pending_spaces.erase(it);
            }
        }
    }
    log("eye gaze bindings suggested: runtime result %d, %d gaze pose action(s) recorded", static_cast<int>(r), recorded);
    // A runtime that advertises the extension but rejects its own profile
    // would leave the app without gaze; the layer can still supply it.
    if (r == XR_ERROR_PATH_UNSUPPORTED && recorded > 0) return XR_SUCCESS;
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_action_space(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    const Dispatch* d = nullptr;
    bool is_gaze = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto s = g_sessions.find(session);
        if (s != g_sessions.end()) d = dispatch_for_instance(s->second.instance);
        else if (!g_instances.empty()) d = &g_instances.begin()->second;
        if (info) is_gaze = g_gaze_actions.count(info->action) != 0;
    }
    if (!d) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = d->create_action_space(session, info, space);
    if (XR_SUCCEEDED(r) && info && space) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const GazeSpace gs{session, info->poseInActionSpace, info->action};
        if (is_gaze) {
            g_gaze_spaces[*space] = gs;
            log("gaze action space created (%zu tracked)", g_gaze_spaces.size());
        } else {
            g_pending_spaces[*space] = gs;
        }
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_space(XrSpace space) {
    const Dispatch* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_gaze_spaces.erase(space);
        g_pending_spaces.erase(space);
        if (!g_instances.empty()) d = &g_instances.begin()->second;
    }
    if (!d) return XR_ERROR_HANDLE_INVALID;
    return d->destroy_space(space);
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_session(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
    const Dispatch* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        d = dispatch_for_instance(instance);
    }
    if (!d) return XR_ERROR_HANDLE_INVALID;
    const XrResult r = d->create_session(instance, info, session);
    if (XR_SUCCEEDED(r) && session) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sessions[*session] = SessionState{instance};
        log("session created");
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_session(XrSession session) {
    const Dispatch* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto s = g_sessions.find(session);
        if (s != g_sessions.end()) d = dispatch_for_instance(s->second.instance);
        g_sessions.erase(session);
        for (auto it = g_gaze_spaces.begin(); it != g_gaze_spaces.end();) {
            it = it->second.session == session ? g_gaze_spaces.erase(it) : std::next(it);
        }
        for (auto it = g_pending_spaces.begin(); it != g_pending_spaces.end();) {
            it = it->second.session == session ? g_pending_spaces.erase(it) : std::next(it);
        }
    }
    if (!d) return XR_ERROR_HANDLE_INVALID;
    return d->destroy_session(session);
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_instance(XrInstance instance) {
    PFN_xrDestroyInstance next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) next = it->second.destroy_instance;
        g_instances.erase(instance);
        if (g_instances.empty()) { g_gaze_actions.clear(); g_gaze_spaces.clear(); g_pending_spaces.clear(); g_sessions.clear(); }
    }
    log("instance destroyed");
    return next ? next(instance) : XR_ERROR_HANDLE_INVALID;
}

std::vector<HMODULE> loaded_modules() {
    std::vector<HMODULE> modules(256);
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) return {};
    if (bytes > modules.size() * sizeof(HMODULE)) {
        modules.resize(bytes / sizeof(HMODULE));
        if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) return {};
    }
    modules.resize(std::min(modules.size(), static_cast<size_t>(bytes / sizeof(HMODULE))));
    return modules;
}

// The loader is the module that exports the two global entry points without
// negotiating as a layer or a runtime. By name first, then by exports, for the
// applications that carry it under another name.
PFN_xrGetInstanceProcAddr find_loader_get_instance_proc_addr() {
    const auto loader_entry = [](HMODULE module) -> PFN_xrGetInstanceProcAddr {
        if (!module || GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface") ||
            GetProcAddress(module, "xrNegotiateLoaderRuntimeInterface") ||
            !GetProcAddress(module, "xrEnumerateApiLayerProperties")) {
            return nullptr;
        }
        return reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(module, "xrGetInstanceProcAddr"));
    };
    if (const auto entry = loader_entry(GetModuleHandleW(L"openxr_loader.dll"))) return entry;
    for (const HMODULE module : loaded_modules()) {
        if (const auto entry = loader_entry(module)) return entry;
    }
    return nullptr;
}

// An application that links the loader statically (R.E.A.L. VR carries it
// inside its dxgi.dll proxy) exports no loader entry point. The runtime is
// already loaded by the time the layer chain is created, and negotiating with
// it again only hands back its entry point, as it did for the loader.
PFN_xrGetInstanceProcAddr find_runtime_get_instance_proc_addr() {
    for (const HMODULE module : loaded_modules()) {
        const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
            GetProcAddress(module, "xrNegotiateLoaderRuntimeInterface"));
        if (!negotiate || GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface")) continue;
        XrNegotiateLoaderInfo info{};
        info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        info.structSize = sizeof(info);
        info.minInterfaceVersion = 1;
        info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
        info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
        XrNegotiateRuntimeRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
        request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        if (XR_SUCCEEDED(negotiate(&info, &request)) && request.getInstanceProcAddr) return request.getInstanceProcAddr;
    }
    return nullptr;
}

// A layer above this one may ask for the global functions before any instance
// exists. Cheeky Foveated DLSS's layer probes xrEnumerateInstanceExtensionProperties
// through the next xrGetInstanceProcAddr with XR_NULL_HANDLE, the way the loader
// probes a runtime, to decide whether to enable XR_EXT_eye_gaze_interaction.
// Answering "invalid handle" made it drop the extension, so the app never bound
// a gaze action and this layer had nothing to rewrite. There is no next entry
// point yet - the loader hands one over only with xrCreateApiLayerInstance - so
// the global functions come from the loader itself, which is what the
// application reaches with the same call.
XrResult resolve_global_function(const char* name, PFN_xrVoidFunction* function) {
    *function = nullptr;
    if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") != 0 &&
        std::strcmp(name, "xrEnumerateApiLayerProperties") != 0) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    PFN_xrGetInstanceProcAddr source = find_loader_get_instance_proc_addr();
    const char* from = "loader";
    if (!source) {
        source = find_runtime_get_instance_proc_addr();
        from = "runtime";
    }
    if (!source) {
        log("null-instance %s: neither loader nor runtime found", name);
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = source(XR_NULL_HANDLE, name, function);
    if (XR_FAILED(result) || !*function) {
        log("null-instance %s: %s answered %d", name, from, static_cast<int>(result));
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    log("null-instance %s: answered from the %s", name, from);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
    if (instance == XR_NULL_HANDLE && std::strcmp(name, "xrGetInstanceProcAddr") != 0) {
        return resolve_global_function(name, function);
    }
    struct Entry { const char* name; PFN_xrVoidFunction fn; };
    static const Entry entries[] = {
        {"xrGetInstanceProcAddr", reinterpret_cast<PFN_xrVoidFunction>(layer_get_instance_proc_addr)},
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(layer_destroy_instance)},
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(layer_create_session)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(layer_destroy_session)},
        {"xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction>(layer_suggest_bindings)},
        {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(layer_create_action_space)},
        {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(layer_destroy_space)},
        {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(layer_locate_space)},
        {"xrGetActionStatePose", reinterpret_cast<PFN_xrVoidFunction>(layer_get_action_state_pose)},
    };
    for (const Entry& e : entries) {
        if (std::strcmp(name, e.name) == 0) { *function = e.fn; return XR_SUCCESS; }
    }
    const Dispatch* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        d = dispatch_for_instance(instance);
        if (!d && !g_instances.empty()) d = &g_instances.begin()->second;
    }
    if (std::strcmp(name, "xrLocateSpaces") == 0) {
        if (d && d->locate_spaces) { *function = reinterpret_cast<PFN_xrVoidFunction>(layer_locate_spaces); return XR_SUCCESS; }
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (!d) { *function = nullptr; return XR_ERROR_HANDLE_INVALID; }
    return d->get_instance_proc_addr(instance, name, function);
}

template <typename T>
bool load(PFN_xrGetInstanceProcAddr gipa, XrInstance inst, const char* name, T& fn) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(gipa(inst, name, &f)) || !f) return false;
    fn = reinterpret_cast<T>(f);
    return true;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer_info, XrInstance* instance) {
    if (!info || !layer_info || !instance || layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        !layer_info->nextInfo || layer_info->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        std::strcmp(layer_info->nextInfo->layerName, kLayerName) != 0 || !layer_info->nextInfo->nextGetInstanceProcAddr ||
        !layer_info->nextInfo->nextCreateApiLayerInstance) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    XrApiLayerCreateInfo next_info = *layer_info;
    next_info.nextInfo = layer_info->nextInfo->next;
    const PFN_xrGetInstanceProcAddr gipa = layer_info->nextInfo->nextGetInstanceProcAddr;
    const XrResult r = layer_info->nextInfo->nextCreateApiLayerInstance(info, &next_info, instance);
    if (XR_FAILED(r)) return r;

    Dispatch d;
    d.get_instance_proc_addr = gipa;
    const bool ok = load(gipa, *instance, "xrDestroyInstance", d.destroy_instance) &&
                    load(gipa, *instance, "xrCreateSession", d.create_session) &&
                    load(gipa, *instance, "xrDestroySession", d.destroy_session) &&
                    load(gipa, *instance, "xrCreateReferenceSpace", d.create_reference_space) &&
                    load(gipa, *instance, "xrCreateActionSpace", d.create_action_space) &&
                    load(gipa, *instance, "xrDestroySpace", d.destroy_space) &&
                    load(gipa, *instance, "xrLocateSpace", d.locate_space) &&
                    load(gipa, *instance, "xrSuggestInteractionProfileBindings", d.suggest_bindings) &&
                    load(gipa, *instance, "xrGetActionStatePose", d.get_action_state_pose) &&
                    load(gipa, *instance, "xrPathToString", d.path_to_string);
    load(gipa, *instance, "xrLocateSpaces", d.locate_spaces);   // optional (1.1)
    if (!ok) {
        log("a required next-layer entry point is missing; the layer stays passive for this instance");
    }

    char env[8] = "";
    if (GetEnvironmentVariableA("PIMAXGAZE_OVERRIDE", env, sizeof env) > 0 && env[0] == '0') g_override = false;

    bool app_enabled_gaze = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (info->enabledExtensionNames[i] && std::strcmp(info->enabledExtensionNames[i], "XR_EXT_eye_gaze_interaction") == 0) app_enabled_gaze = true;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[*instance] = d;
    }
    PFN_xrGetInstanceProperties get_props = nullptr;
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (load(gipa, *instance, "xrGetInstanceProperties", get_props)) get_props(*instance, &props);
    log("instance created for '%s' on runtime '%s'; app enabled eye gaze: %s; override: %s; xrLocateSpaces: %s",
        info->applicationInfo.applicationName, props.runtimeName, app_enabled_gaze ? "yes" : "no",
        g_override ? "on" : "off (PIMAXGAZE_OVERRIDE=0)", d.locate_spaces ? "yes" : "no");
    return r;
}

}  // namespace

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader_info, const char* layer_name, XrNegotiateApiLayerRequest* request) {
    if (!loader_info || !layer_name || !request || loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader_info->structSize != sizeof(XrNegotiateLoaderInfo) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST || request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize != sizeof(XrNegotiateApiLayerRequest) || std::strcmp(layer_name, kLayerName) != 0 ||
        loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION || loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader_info->maxApiVersion < kLayerApiVersion) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = kLayerApiVersion;
    request->getInstanceProcAddr = layer_get_instance_proc_addr;
    request->createApiLayerInstance = layer_create_api_layer_instance;
    log("negotiated with the loader");
    return XR_SUCCESS;
}
