// Pimax gaze calibration recorder.
//
// A minimal OpenXR + D3D11 application that shows a fixation target at known
// angles relative to the head (head-locked, in the VIEW reference space),
// holds each for a fixed time, and logs the schedule against the Windows
// QueryPerformanceCounter clock in microseconds. BrokenEye recordings carry
// the same clock in `TimestampSystemUs`, so camera frames can be labelled
// with the target being fixated at that instant.
//
// While it runs it also reads the runtime's own eye gaze through
// XR_EXT_eye_gaze_interaction (when the runtime offers it) and logs it per
// frame, giving Tobii's view of the same session with proper timestamps.
//
// Conventions in the logs (all in the VIEW space, i.e. the head frame):
//   +X right, +Y up, -Z forward. yaw is positive to the right, pitch positive
//   up, both in degrees. Directions: x = sin(yaw) cos(pitch), y = sin(pitch),
//   z = -cos(yaw) cos(pitch).
//
// Usage: calib_recorder.exe [--passes N] [--hold S] [--move S] [--distance M]
//        calib_recorder.exe --follow [--distance M]
// --follow shows the calibration positions faintly and a bright dot where
// the runtime (or the gaze layer above it) says the eyes are looking: green
// while tracked, red while not. Escape ends it. Logs go to the current directory.

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

using namespace DirectX;

namespace {

// ---------------------------------------------------------------- utilities

double g_qpcToUs = 0.0;

long long nowUs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<long long>(static_cast<double>(c.QuadPart) * g_qpcToUs);
}

std::string utcNow() {
    const auto t = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
    return buf;
}

std::string localStamp() {
    const std::time_t tt = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }
double rad2deg(double r) { return r * 180.0 / kPi; }

XrVector3f dirFromAngles(double yawDeg, double pitchDeg) {
    const double y = deg2rad(yawDeg), p = deg2rad(pitchDeg);
    return {static_cast<float>(std::sin(y) * std::cos(p)), static_cast<float>(std::sin(p)),
            static_cast<float>(-std::cos(y) * std::cos(p))};
}

void anglesFromDir(const XrVector3f& d, double& yawDeg, double& pitchDeg) {
    yawDeg = rad2deg(std::atan2(d.x, -d.z));
    pitchDeg = rad2deg(std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z)));
}

XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XMVECTOR qv = XMVectorSet(q.x, q.y, q.z, q.w);
    const XMVECTOR r = XMVector3Rotate(XMVectorSet(v.x, v.y, v.z, 0.f), qv);
    return {XMVectorGetX(r), XMVectorGetY(r), XMVectorGetZ(r)};
}

// ---------------------------------------------------------------- OpenXR fns

PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
PFN_xrCreateInstance xrCreateInstance = nullptr;
PFN_xrDestroyInstance xrDestroyInstance = nullptr;
PFN_xrGetInstanceProperties xrGetInstanceProperties = nullptr;
PFN_xrResultToString xrResultToString = nullptr;
PFN_xrGetSystem xrGetSystem = nullptr;
PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews = nullptr;
PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR = nullptr;
PFN_xrCreateSession xrCreateSession = nullptr;
PFN_xrDestroySession xrDestroySession = nullptr;
PFN_xrCreateReferenceSpace xrCreateReferenceSpace = nullptr;
PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = nullptr;
PFN_xrCreateSwapchain xrCreateSwapchain = nullptr;
PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
PFN_xrPollEvent xrPollEvent = nullptr;
PFN_xrBeginSession xrBeginSession = nullptr;
PFN_xrEndSession xrEndSession = nullptr;
PFN_xrRequestExitSession xrRequestExitSession = nullptr;
PFN_xrWaitFrame xrWaitFrame = nullptr;
PFN_xrBeginFrame xrBeginFrame = nullptr;
PFN_xrEndFrame xrEndFrame = nullptr;
PFN_xrLocateViews xrLocateViews = nullptr;
PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = nullptr;
PFN_xrWaitSwapchainImage xrWaitSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = nullptr;
PFN_xrStringToPath xrStringToPath = nullptr;
PFN_xrCreateActionSet xrCreateActionSet = nullptr;
PFN_xrCreateAction xrCreateAction = nullptr;
PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings = nullptr;
PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
PFN_xrCreateActionSpace xrCreateActionSpace = nullptr;
PFN_xrSyncActions xrSyncActions = nullptr;
PFN_xrGetActionStatePose xrGetActionStatePose = nullptr;
PFN_xrLocateSpace xrLocateSpace = nullptr;
PFN_xrConvertTimeToWin32PerformanceCounterKHR xrConvertTimeToWin32PerformanceCounterKHR = nullptr;

XrInstance g_instance = XR_NULL_HANDLE;

template <typename T>
bool load(XrInstance inst, const char* name, T& fn) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(inst, name, &f)) || !f) {
        return false;
    }
    fn = reinterpret_cast<T>(f);
    return true;
}

[[noreturn]] void die(const char* what, XrResult r = XR_SUCCESS) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = "";
    if (r != XR_SUCCESS && g_instance != XR_NULL_HANDLE && xrResultToString) {
        xrResultToString(g_instance, r, buf);
    }
    std::fprintf(stderr, "FATAL: %s%s%s (%d)\n", what, buf[0] ? ": " : "", buf, static_cast<int>(r));
    std::fflush(stderr);
    std::exit(1);
}

#define CHECK(call)                                          \
    do {                                                     \
        const XrResult r_ = (call);                          \
        if (XR_FAILED(r_)) die(#call, r_);                   \
    } while (0)

// ---------------------------------------------------------------- shaders

const char* kShader = R"(
cbuffer Constants : register(b0) {
    row_major float4x4 viewProj;
    float3 center; float radius;
    float3 axisRight; float pad0;
    float3 axisUp; float pad1;
    float4 color;
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    float2 c = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? 1.0 : -1.0);
    float3 world = center + axisRight * (c.x * radius) + axisUp * (c.y * radius);
    VSOut o;
    o.pos = mul(float4(world, 1.0), viewProj);
    o.uv = c;
    return o;
}
float4 ps(VSOut i) : SV_Target {
    float r = length(i.uv);
    if (r > 1.0) discard;
    // Bright ring with a dark centre: a classic fixation target.
    float v = (r < 0.28) ? 0.02 : 1.0;
    return float4(color.rgb * v, 1.0);
}
)";

struct Constants {
    XMFLOAT4X4 viewProj;
    XMFLOAT3 center; float radius;
    XMFLOAT3 axisRight; float pad0;
    XMFLOAT3 axisUp; float pad1;
    XMFLOAT4 color;
};

struct Dot {
    XrVector3f dir;
    float radiusDeg;
    XMFLOAT4 color;
};

// ---------------------------------------------------------------- schedule

struct Step {
    std::string label;  // intro, p1, p2, ..., close
    int pass;           // 0 for intro/close
    int index;          // target index in the grid (0 = centre)
    double yaw, pitch;
    double moveSec, holdSec;
};

// 15-point grid, widened on 2026-09-27 after the 13-point +/-20 deg grid left
// far left/right gaze to extrapolation (the quadratic maps fold over there):
// centre, inner square (+/-12, +/-8), middle cross (+/-20, 0) and (0, +/-20),
// outer points (+/-30, 0) and corners (+/-30, +/-20).
const double kGrid[][2] = {
    {0, 0},
    {-12, 8}, {12, 8}, {-12, -8}, {12, -8},
    {-20, 0}, {20, 0}, {0, 20}, {0, -20},
    {-30, 0}, {30, 0},
    {-30, 20}, {30, 20}, {-30, -20}, {30, -20},
};
constexpr int kGridCount = static_cast<int>(sizeof(kGrid) / sizeof(kGrid[0]));

// Visiting order alternates sides so that any slow drift is not confounded
// with position. Index into kGrid.
const int kOrder[kGridCount] = {0, 6, 5, 7, 8, 10, 9, 2, 3, 1, 4, 12, 13, 11, 14};

std::vector<Step> buildSchedule(int passes, double holdSec, double moveSec) {
    std::vector<Step> s;
    s.push_back({"intro", 0, 0, 0, 0, 0.0, 3.0});
    for (int p = 1; p <= passes; ++p) {
        for (int k = 0; k < kGridCount; ++k) {
            const int i = kOrder[k];
            s.push_back({"p" + std::to_string(p), p, i, kGrid[i][0], kGrid[i][1], moveSec, holdSec});
        }
    }
    s.push_back({"close", 0, 0, 0, 0, moveSec, holdSec});
    return s;
}

double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}

}  // namespace

int main(int argc, char** argv) {
    int passes = 2;
    double holdSec = 2.0, moveSec = 0.6, distance = 1.5;
    bool follow = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](double& v) { if (i + 1 < argc) v = std::atof(argv[++i]); };
        if (a == "--passes" && i + 1 < argc) passes = std::atoi(argv[++i]);
        else if (a == "--hold") next(holdSec);
        else if (a == "--move") next(moveSec);
        else if (a == "--distance") next(distance);
        else if (a == "--follow") follow = true;
        else { std::printf("usage: calib_recorder [--passes N] [--hold S] [--move S] [--distance M] | --follow\n"); return 0; }
    }
    passes = std::clamp(passes, 1, 10);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // status lines must show up even when piped

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcToUs = 1e6 / static_cast<double>(freq.QuadPart);

    // Keep the gaze-consuming layers out of this process: the recorder wants the
    // runtime's own eye gaze, not something a bridge layer has already touched.
    SetEnvironmentVariableA("CHEEKY_OPENXR_LAYER_DISABLE", "1");

    HMODULE loader = LoadLibraryW(L"openxr_loader.dll");
    if (!loader) die("openxr_loader.dll not found next to the executable");
    xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
    if (!xrGetInstanceProcAddr) die("xrGetInstanceProcAddr missing from openxr_loader.dll");
    load(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", xrEnumerateInstanceExtensionProperties);
    load(XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstance);

    // ---- instance
    uint32_t extCount = 0;
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr));
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()));
    auto hasExt = [&](const char* n) {
        return std::any_of(exts.begin(), exts.end(), [&](const XrExtensionProperties& e) { return std::strcmp(e.extensionName, n) == 0; });
    };
    if (!hasExt(XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) die("runtime lacks XR_KHR_D3D11_enable");
    const bool haveGaze = hasExt(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    const bool haveConvert = hasExt(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    std::vector<const char*> enabled = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (haveGaze) enabled.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    if (haveConvert) enabled.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ici.applicationInfo.applicationName, "PimaxGazeCalibRecorder");
    ici.applicationInfo.applicationVersion = 1;
    std::strcpy(ici.applicationInfo.engineName, "none");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ici.enabledExtensionNames = enabled.data();
    CHECK(xrCreateInstance(&ici, &g_instance));

#define LOAD(name) if (!load(g_instance, #name, name)) die("missing " #name)
    LOAD(xrDestroyInstance); LOAD(xrGetInstanceProperties); LOAD(xrResultToString); LOAD(xrGetSystem);
    LOAD(xrGetSystemProperties); LOAD(xrEnumerateViewConfigurationViews); LOAD(xrGetD3D11GraphicsRequirementsKHR);
    LOAD(xrCreateSession); LOAD(xrDestroySession); LOAD(xrCreateReferenceSpace); LOAD(xrEnumerateSwapchainFormats);
    LOAD(xrCreateSwapchain); LOAD(xrEnumerateSwapchainImages); LOAD(xrPollEvent); LOAD(xrBeginSession);
    LOAD(xrEndSession); LOAD(xrRequestExitSession); LOAD(xrWaitFrame); LOAD(xrBeginFrame); LOAD(xrEndFrame);
    LOAD(xrLocateViews); LOAD(xrAcquireSwapchainImage); LOAD(xrWaitSwapchainImage); LOAD(xrReleaseSwapchainImage);
    LOAD(xrStringToPath); LOAD(xrCreateActionSet); LOAD(xrCreateAction); LOAD(xrSuggestInteractionProfileBindings);
    LOAD(xrAttachSessionActionSets); LOAD(xrCreateActionSpace); LOAD(xrSyncActions); LOAD(xrGetActionStatePose);
    LOAD(xrLocateSpace);
    if (haveConvert) LOAD(xrConvertTimeToWin32PerformanceCounterKHR);
#undef LOAD

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    CHECK(xrGetInstanceProperties(g_instance, &ip));
    std::printf("runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    std::printf("eye gaze extension: %s, QPC time conversion: %s\n", haveGaze ? "yes" : "NO", haveConvert ? "yes" : "no");

    // ---- system
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    CHECK(xrGetSystem(g_instance, &sgi, &system));
    XrSystemEyeGazeInteractionPropertiesEXT gazeProps{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (haveGaze) sp.next = &gazeProps;
    CHECK(xrGetSystemProperties(g_instance, system, &sp));
    std::printf("system: %s, supportsEyeGazeInteraction=%d\n", sp.systemName, haveGaze ? gazeProps.supportsEyeGazeInteraction : 0);

    uint32_t viewCount = 0;
    CHECK(xrEnumerateViewConfigurationViews(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));
    if (viewCount != 2) die("expected a stereo view configuration");
    std::vector<XrViewConfigurationView> vcv(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    CHECK(xrEnumerateViewConfigurationViews(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, vcv.data()));

    // ---- D3D11
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    CHECK(xrGetD3D11GraphicsRequirementsKHR(g_instance, system, &req));
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) die("CreateDXGIFactory1");
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (std::memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) break;
        adapter->Release();
        adapter = nullptr;
    }
    if (!adapter) die("adapter with the runtime's LUID not found");
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = req.minFeatureLevel;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        die("D3D11CreateDevice");
    }

    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), "calib", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err))) {
        std::fprintf(stderr, "%s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        die("vertex shader");
    }
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), "calib", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        std::fprintf(stderr, "%s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        die("pixel shader");
    }
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Constants);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* cb = nullptr;
    device->CreateBuffer(&cbd, nullptr, &cb);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr;
    device->CreateRasterizerState(&rd, &rs);

    // ---- session, spaces
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    CHECK(xrCreateSession(g_instance, &sci, &session));

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rsci.poseInReferenceSpace.orientation.w = 1.f;
    XrSpace viewSpace = XR_NULL_HANDLE;
    CHECK(xrCreateReferenceSpace(session, &rsci, &viewSpace));

    // ---- eye gaze action (optional)
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction gazeAction = XR_NULL_HANDLE;
    XrSpace gazeSpace = XR_NULL_HANDLE;
    bool gazeBound = false;
    if (haveGaze && gazeProps.supportsEyeGazeInteraction) {
        XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(asci.actionSetName, "calib");
        std::strcpy(asci.localizedActionSetName, "Calibration");
        CHECK(xrCreateActionSet(g_instance, &asci, &actionSet));
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(aci.actionName, "gaze");
        std::strcpy(aci.localizedActionName, "Gaze");
        aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        CHECK(xrCreateAction(actionSet, &aci, &gazeAction));
        XrPath profile, gazePath;
        CHECK(xrStringToPath(g_instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile));
        CHECK(xrStringToPath(g_instance, "/user/eyes_ext/input/gaze_ext/pose", &gazePath));
        XrActionSuggestedBinding b{gazeAction, gazePath};
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = profile;
        sb.countSuggestedBindings = 1;
        sb.suggestedBindings = &b;
        const XrResult sr = xrSuggestInteractionProfileBindings(g_instance, &sb);
        if (XR_SUCCEEDED(sr)) {
            XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            at.countActionSets = 1;
            at.actionSets = &actionSet;
            CHECK(xrAttachSessionActionSets(session, &at));
            XrActionSpaceCreateInfo asp{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            asp.action = gazeAction;
            asp.poseInActionSpace.orientation.w = 1.f;
            CHECK(xrCreateActionSpace(session, &asp, &gazeSpace));
            gazeBound = true;
        } else {
            std::printf("eye gaze binding rejected (%d); continuing without runtime gaze\n", static_cast<int>(sr));
        }
    }

    // ---- swapchains
    uint32_t fmtCount = 0;
    CHECK(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr));
    std::vector<int64_t> fmts(fmtCount);
    CHECK(xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, fmts.data()));
    const int64_t prefer[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    int64_t fmt = 0;
    for (int64_t p : prefer) {
        if (std::find(fmts.begin(), fmts.end(), p) != fmts.end()) { fmt = p; break; }
    }
    if (!fmt) die("no usable swapchain format");

    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        std::vector<ID3D11RenderTargetView*> rtvs;
    } eyes[2];
    for (uint32_t v = 0; v < 2; ++v) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc.format = fmt;
        sc.sampleCount = 1;
        sc.width = vcv[v].recommendedImageRectWidth;
        sc.height = vcv[v].recommendedImageRectHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        CHECK(xrCreateSwapchain(session, &sc, &eyes[v].swapchain));
        eyes[v].w = sc.width;
        eyes[v].h = sc.height;
        uint32_t n = 0;
        CHECK(xrEnumerateSwapchainImages(eyes[v].swapchain, 0, &n, nullptr));
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        CHECK(xrEnumerateSwapchainImages(eyes[v].swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));
        for (auto& im : imgs) {
            D3D11_RENDER_TARGET_VIEW_DESC rtd{};
            rtd.Format = static_cast<DXGI_FORMAT>(fmt);
            rtd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            if (FAILED(device->CreateRenderTargetView(im.texture, &rtd, &rtv))) die("CreateRenderTargetView");
            eyes[v].rtvs.push_back(rtv);
        }
    }
    std::printf("swapchains: %ux%u per eye, format %lld\n", eyes[0].w, eyes[0].h, static_cast<long long>(fmt));

    // ---- logs
    const std::string stamp = localStamp();
    std::ofstream sched("calib-" + stamp + "-schedule.csv");
    std::ofstream frames("calib-" + stamp + "-frames.csv");
    sched << "# Pimax gaze calibration recorder. qpc_us = QueryPerformanceCounter in microseconds "
             "(same clock as BrokenEye TimestampSystemUs). Angles in degrees, head frame: yaw + right, pitch + up.\n"
          << "# runtime=" << ip.runtimeName << " passes=" << passes << " hold_s=" << holdSec << " move_s=" << moveSec
          << " distance_m=" << distance << "\n"
          << "event,qpc_us,utc,label,pass,index,yaw,pitch,x,y,z\n";
    frames << "# Per-frame log. display_qpc_us is the predicted display time converted to the QPC clock (blank if unsupported). "
              "Gaze is the runtime's XR_EXT_eye_gaze_interaction pose located in VIEW space at display time.\n"
           << "frame,qpc_us,display_qpc_us,label,pass,index,phase,tgt_yaw,tgt_pitch,tgt_x,tgt_y,tgt_z,"
              "eyeL_x,eyeL_y,eyeL_z,eyeR_x,eyeR_y,eyeR_z,gaze_active,gaze_flags,gaze_yaw,gaze_pitch,gaze_x,gaze_y,gaze_z\n";
    auto logEvent = [&](const char* ev, const Step& s, const XrVector3f& p) {
        sched << ev << ',' << nowUs() << ',' << utcNow() << ',' << s.label << ',' << s.pass << ',' << s.index << ','
              << s.yaw << ',' << s.pitch << ',' << p.x << ',' << p.y << ',' << p.z << '\n';
        sched.flush();
    };

    const std::vector<Step> steps = follow ? std::vector<Step>{} : buildSchedule(passes, holdSec, moveSec);
    const double totalSec = [&] { double t = 0; for (auto& s : steps) t += s.moveSec + s.holdSec; return t; }();
    if (follow) {
        std::printf("follow mode: the bright dot follows the eye gaze the runtime reports (green = tracked, red = not). Escape ends.\n");
    } else {
        std::printf("schedule: %zu steps, %.0f s. Put the headset on; the sequence starts when the session is running. Escape aborts.\n",
                    steps.size(), totalSec);
    }
    long long lastFollowPrintUs = 0;
    XrVector3f lastGazeDir{0, 0, -1};

    // ---- main loop
    bool running = false, quit = false, exitRequested = false;
    long long seqStartUs = -1;
    size_t stepIdx = 0;
    double stepStartSec = 0.0;
    bool holdLogged = false;
    double prevYaw = 0, prevPitch = 0;
    uint64_t frameNo = 0;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;

    auto requestExit = [&](const char* why) {
        if (!exitRequested) {
            std::printf("%s\n", why);
            exitRequested = true;
            xrRequestExitSession(session);
        }
    };

    while (!quit) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CHECK(xrBeginSession(session, &bi));
                    running = true;
                    std::printf("session running\n");
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    running = false;
                    CHECK(xrEndSession(session));
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit = true;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (quit) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            if (running) {
                requestExit(follow ? "ended by Escape" : "aborted by Escape");
                if (!steps.empty()) {
                    Step s = steps[std::min(stepIdx, steps.size() - 1)];
                    logEvent("abort", s, {});
                }
            } else {
                quit = true;
            }
        }
        if (!running) {
            Sleep(10);
            continue;
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        CHECK(xrWaitFrame(session, nullptr, &fs));
        CHECK(xrBeginFrame(session, nullptr));
        ++frameNo;

        // -- advance the schedule on the QPC clock
        const long long tNow = nowUs();
        bool finished = false;
        XrVector3f targetDir{0, 0, -1};
        const char* phase = "wait";
        const Step* cur = nullptr;
        if (fs.shouldRender && seqStartUs < 0) {
            seqStartUs = tNow;
            std::printf("sequence started\n");
        }
        if (seqStartUs >= 0 && stepIdx < steps.size()) {
            double t = (tNow - seqStartUs) / 1e6 - stepStartSec;
            // roll over completed steps
            while (stepIdx < steps.size() && t >= steps[stepIdx].moveSec + steps[stepIdx].holdSec) {
                const Step& s = steps[stepIdx];
                if (holdLogged) logEvent("hold_end", s, dirFromAngles(s.yaw, s.pitch));
                stepStartSec += s.moveSec + s.holdSec;
                t -= s.moveSec + s.holdSec;
                prevYaw = s.yaw;
                prevPitch = s.pitch;
                ++stepIdx;
                holdLogged = false;
            }
            if (stepIdx < steps.size()) {
                cur = &steps[stepIdx];
                double yaw = cur->yaw, pitch = cur->pitch;
                if (t < cur->moveSec) {
                    const double a = smoothstep(t / cur->moveSec);
                    yaw = prevYaw + (cur->yaw - prevYaw) * a;
                    pitch = prevPitch + (cur->pitch - prevPitch) * a;
                    phase = "move";
                } else {
                    phase = "hold";
                    if (!holdLogged) {
                        logEvent("hold_start", *cur, dirFromAngles(cur->yaw, cur->pitch));
                        std::printf("  %s target %d (yaw %+.1f pitch %+.1f)\n", cur->label.c_str(), cur->index, cur->yaw, cur->pitch);
                        holdLogged = true;
                    }
                }
                targetDir = dirFromAngles(yaw, pitch);
            } else {
                finished = true;
            }
        }
        if (finished && !follow) {
            Step end{"end", 0, 0, 0, 0, 0, 0};
            logEvent("end", end, {});
            requestExit("sequence complete");
        }

        // -- views, gaze
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = viewSpace;
        XrViewState vstate{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t nViews = 0;
        CHECK(xrLocateViews(session, &vli, &vstate, 2, &nViews, views));

        bool gazeActive = false;
        XrSpaceLocation gazeLoc{XR_TYPE_SPACE_LOCATION};
        if (gazeBound) {
            XrActiveActionSet active{actionSet, XR_NULL_PATH};
            XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
            si.countActiveActionSets = 1;
            si.activeActionSets = &active;
            const XrResult syncR = xrSyncActions(session, &si);
            if (XR_SUCCEEDED(syncR)) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = gazeAction;
                XrActionStatePose st{XR_TYPE_ACTION_STATE_POSE};
                if (XR_SUCCEEDED(xrGetActionStatePose(session, &gi, &st))) gazeActive = st.isActive;
                if (XR_FAILED(xrLocateSpace(gazeSpace, viewSpace, fs.predictedDisplayTime, &gazeLoc))) gazeLoc.locationFlags = 0;
            }
        }

        // -- log the frame
        {
            std::string dispUs;
            if (haveConvert) {
                LARGE_INTEGER li;
                if (XR_SUCCEEDED(xrConvertTimeToWin32PerformanceCounterKHR(g_instance, fs.predictedDisplayTime, &li))) {
                    dispUs = std::to_string(static_cast<long long>(static_cast<double>(li.QuadPart) * g_qpcToUs));
                }
            }
            double ty = 0, tp = 0;
            anglesFromDir(targetDir, ty, tp);
            const XrVector3f tp3 = {targetDir.x * static_cast<float>(distance), targetDir.y * static_cast<float>(distance), targetDir.z * static_cast<float>(distance)};
            double gy = 0, gp = 0;
            XrVector3f gdir{0, 0, 0};
            const bool orientValid = (gazeLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
            if (orientValid) {
                gdir = rotate(gazeLoc.pose.orientation, {0, 0, -1});
                anglesFromDir(gdir, gy, gp);
            }
            char line[512];
            std::snprintf(line, sizeof line,
                          "%llu,%lld,%s,%s,%d,%d,%s,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%llu,%.3f,%.3f,%.4f,%.4f,%.4f\n",
                          static_cast<unsigned long long>(frameNo), tNow, dispUs.c_str(), cur ? cur->label.c_str() : "-",
                          cur ? cur->pass : 0, cur ? cur->index : -1, phase, ty, tp, tp3.x, tp3.y, tp3.z,
                          views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
                          views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z,
                          gazeActive ? 1 : 0, static_cast<unsigned long long>(gazeLoc.locationFlags),
                          orientValid ? gy : 0.0, orientValid ? gp : 0.0,
                          gazeLoc.pose.position.x, gazeLoc.pose.position.y, gazeLoc.pose.position.z);
            frames << line;

            if (follow && tNow - lastFollowPrintUs > 1'000'000) {
                lastFollowPrintUs = tNow;
                const bool tracked = (gazeLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
                std::printf("gaze active=%d flags=%llu %s yaw %+6.1f pitch %+6.1f\n", gazeActive ? 1 : 0,
                            static_cast<unsigned long long>(gazeLoc.locationFlags), tracked ? "TRACKED  " : "untracked",
                            orientValid ? gy : 0.0, orientValid ? gp : 0.0);
            }
        }

        // -- what to draw this frame
        std::vector<Dot> dots;
        if (follow) {
            for (const auto& g : kGrid) dots.push_back({dirFromAngles(g[0], g[1]), 0.3f, {0.35f, 0.35f, 0.35f, 1}});
            const bool orientValid = (gazeLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
            const bool tracked = (gazeLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
            if (orientValid) lastGazeDir = rotate(gazeLoc.pose.orientation, {0, 0, -1});
            dots.push_back({lastGazeDir, 0.6f, tracked ? XMFLOAT4{0.2f, 1.f, 0.2f, 1} : XMFLOAT4{1.f, 0.15f, 0.15f, 1}});
        } else if (cur) {
            dots.push_back({targetDir, 0.45f, {1, 1, 1, 1}});
        }

        // -- render
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        uint32_t layerCount = 0;
        if (fs.shouldRender && (vstate.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            for (uint32_t v = 0; v < 2; ++v) {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                CHECK(xrAcquireSwapchainImage(eyes[v].swapchain, &ai, &idx));
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                CHECK(xrWaitSwapchainImage(eyes[v].swapchain, &wi));

                const XrFovf& f = views[v].fov;
                const float tl = std::tan(f.angleLeft), tr = std::tan(f.angleRight), td = std::tan(f.angleDown), tu = std::tan(f.angleUp);
                const float tw = tr - tl, th = tu - td, nearZ = 0.05f, farZ = 100.f;
                XMFLOAT4X4 pf(2 / tw, 0, 0, 0,
                              0, 2 / th, 0, 0,
                              (tr + tl) / tw, (tu + td) / th, -farZ / (farZ - nearZ), -1,
                              0, 0, -(farZ * nearZ) / (farZ - nearZ), 0);
                const XMMATRIX proj = XMLoadFloat4x4(&pf);
                const XrPosef& p = views[v].pose;
                const XMMATRIX world = XMMatrixRotationQuaternion(XMVectorSet(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w)) *
                                       XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
                const XMMATRIX view = XMMatrixInverse(nullptr, world);

                ID3D11RenderTargetView* rtv = eyes[v].rtvs[idx];
                const float clear[4] = {0.02f, 0.02f, 0.02f, 1.f};
                ctx->ClearRenderTargetView(rtv, clear);
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                D3D11_VIEWPORT vp{0, 0, static_cast<float>(eyes[v].w), static_cast<float>(eyes[v].h), 0, 1};
                ctx->RSSetViewports(1, &vp);
                ctx->RSSetState(rs);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                ctx->IASetInputLayout(nullptr);
                ctx->VSSetShader(vs, nullptr, 0);
                ctx->VSSetConstantBuffers(0, 1, &cb);
                ctx->PSSetShader(ps, nullptr, 0);
                ctx->PSSetConstantBuffers(0, 1, &cb);
                for (const Dot& dot : dots) {
                    const XMVECTOR dir = XMVector3Normalize(XMVectorSet(dot.dir.x, dot.dir.y, dot.dir.z, 0));
                    XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), dir));
                    if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-6f) right = XMVectorSet(1, 0, 0, 0);
                    const XMVECTOR up = XMVector3Normalize(XMVector3Cross(dir, right));
                    Constants c{};
                    XMStoreFloat4x4(&c.viewProj, view * proj);
                    c.center = {dot.dir.x * static_cast<float>(distance), dot.dir.y * static_cast<float>(distance), dot.dir.z * static_cast<float>(distance)};
                    c.radius = static_cast<float>(distance * std::tan(deg2rad(dot.radiusDeg)));
                    XMStoreFloat3(&c.axisRight, right);
                    XMStoreFloat3(&c.axisUp, up);
                    c.color = dot.color;
                    ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);
                    ctx->Draw(4, 0);
                }
                ID3D11RenderTargetView* none = nullptr;
                ctx->OMSetRenderTargets(1, &none, nullptr);

                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                CHECK(xrReleaseSwapchainImage(eyes[v].swapchain, &ri));

                pv[v].pose = views[v].pose;
                pv[v].fov = views[v].fov;
                pv[v].subImage.swapchain = eyes[v].swapchain;
                pv[v].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(eyes[v].w), static_cast<int32_t>(eyes[v].h)}};
                pv[v].subImage.imageArrayIndex = 0;
            }
            layer.space = viewSpace;
            layer.viewCount = 2;
            layer.views = pv;
            layerCount = 1;
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layerCount;
        fei.layers = layerCount ? layers : nullptr;
        CHECK(xrEndFrame(session, &fei));
    }

    sched.close();
    frames.close();
    std::printf("logs: calib-%s-schedule.csv, calib-%s-frames.csv\n", stamp.c_str(), stamp.c_str());
    xrDestroySession(session);
    xrDestroyInstance(g_instance);
    return 0;
}
