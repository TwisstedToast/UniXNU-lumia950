#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "Timing.h"
#include "ShaderSource.h"

namespace {

constexpr unsigned kQueries = 16;
constexpr unsigned kHudWidth = 1024;
constexpr unsigned kHudHeight = 256;
UINT64 windowStarted;

template <typename Object> void release(Object *&object)
{
    if (object) { object->Release(); object = nullptr; }
}

uint64_t counter()
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

struct Timestamp {
    ID3D11Query *disjoint;
    ID3D11Query *begin;
    ID3D11Query *end;
    bool pending;
};

struct App {
    FILE *log;
    FILE *csv;
    wchar_t directory[MAX_PATH];
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *target;
    ID3D11VertexShader *vertex;
    ID3D11PixelShader *pixel;
    ID3D11Buffer *uniforms;
    ID3D11Texture2D *hud;
    ID3D11ShaderResourceView *hudView;
    ID3D11SamplerState *sampler;
    Timestamp queries[kQueries];
    ID3DBlob *vertexCode;
    ID3DBlob *pixelCode;
    HDC hudDc;
    HBITMAP hudBitmap;
    HFONT hudFont;
    HGDIOBJ oldBitmap;
    HGDIOBJ oldFont;
    uint32_t *hudPixels;
    HWND window;
    UINT width;
    UINT height;
    unsigned nextQuery;
    unsigned pendingQuery;
    uint64_t gpuSamples;
    double gpuMilliseconds;
    lumia_demo::DisplayRate display;
    DXGI_ADAPTER_DESC1 description;
    bool adreno418;

    ~App()
    {
        if (context) { context->ClearState(); context->Flush(); }
        for (Timestamp &query : queries) {
            release(query.disjoint); release(query.begin); release(query.end);
        }
        release(target); release(vertex); release(pixel); release(uniforms);
        release(hudView); release(hud); release(sampler);
        release(swap); release(context); release(device); release(adapter); release(factory);
        release(vertexCode); release(pixelCode);
        if (hudDc && oldFont) { SelectObject(hudDc, oldFont); }
        if (hudDc && oldBitmap) { SelectObject(hudDc, oldBitmap); }
        if (hudFont) { DeleteObject(hudFont); }
        if (hudBitmap) { DeleteObject(hudBitmap); }
        if (hudDc) { DeleteDC(hudDc); }
        if (window) { DestroyWindow(window); }
        if (csv) { fclose(csv); }
        if (log) { fclose(log); }
    }
};

bool reports(App &app, const wchar_t *parent)
{
    SYSTEMTIME now = {};
    GetSystemTime(&now);
    if (swprintf(app.directory, MAX_PATH, L"%ls\\LumiaGpuDemo-%04u%02u%02u-%02u%02u%02u-%lu",
        parent, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        GetCurrentProcessId()) < 0 || !CreateDirectoryW(app.directory, nullptr)) { return false; }
    wchar_t path[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\report.txt", app.directory) < 0) { return false; }
    app.log = _wfopen(path, L"wb");
    if (swprintf(path, MAX_PATH, L"%ls\\frames.csv", app.directory) < 0) { return false; }
    app.csv = _wfopen(path, L"wb");
    if (!app.log || !app.csv) { return false; }
    setvbuf(app.log, nullptr, _IONBF, 0);
    fprintf(app.log, "Lumia GPU hardware baseline v1\nWindows D3D11; NOT native iOS/XNU.\n");
    fprintf(app.csv, "qpc,submitted,present_hr,stats_hr,present_count,present_refresh_count,"
        "sync_refresh_count,sync_qpc,display_fps,refresh_hz,gpu_ms,gpu_samples\n");
    return true;
}

bool openReports(App &app)
{
    wchar_t location[MAX_PATH] = {};
    const DWORD count = GetModuleFileNameW(nullptr, location, MAX_PATH);
    if (!count || count >= MAX_PATH) { return false; }
    wchar_t *separator = wcsrchr(location, L'\\');
    if (!separator) { return false; }
    *separator = 0;
    if (reports(app, location)) { return true; }
    if (app.csv) { fclose(app.csv); app.csv = nullptr; }
    if (app.log) { fclose(app.log); app.log = nullptr; }
    const DWORD length = GetTempPathW(MAX_PATH, location);
    return length && length < MAX_PATH && reports(app, location);
}

bool succeeded(App &app, HRESULT result, const char *operation)
{
    if (SUCCEEDED(result)) { return true; }
    fprintf(app.log, "FAILED %s HRESULT=0x%08lx\n", operation, static_cast<unsigned long>(result));
    if (app.device) {
        fprintf(app.log, "DeviceRemovedReason=0x%08lx\n",
            static_cast<unsigned long>(app.device->GetDeviceRemovedReason()));
    }
    return false;
}

bool compile(App &app, const char *entry, const char *profile, ID3DBlob **code)
{
    ID3DBlob *errors = nullptr;
    const HRESULT result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "Animation.hlsl",
        nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, code, &errors);
    if (errors) {
        fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), app.log);
        release(errors);
    }
    return succeeded(app, result, entry);
}

bool selectAdapter(App &app, bool otherHardware)
{
    if (!succeeded(app, CreateDXGIFactory1(__uuidof(IDXGIFactory1),
        reinterpret_cast<void **>(&app.factory)), "CreateDXGIFactory1")) { return false; }
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1 *candidate = nullptr;
        const HRESULT result = app.factory->EnumAdapters1(index, &candidate);
        if (result == DXGI_ERROR_NOT_FOUND) { break; }
        if (!succeeded(app, result, "EnumAdapters1")) { return false; }
        DXGI_ADAPTER_DESC1 description = {};
        const HRESULT described = candidate->GetDesc1(&description);
        if (FAILED(described)) { release(candidate); return succeeded(app, described, "GetDesc1"); }
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name), nullptr, nullptr);
        fprintf(app.log, "Adapter[%u]=%s vendor=0x%04x device=0x%08x flags=0x%x\n",
            index, name, description.VendorId, description.DeviceId, description.Flags);
        const bool hardware = !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        const bool adreno418 = description.VendorId == 0x5143 &&
            wcsstr(description.Description, L"Adreno") && wcsstr(description.Description, L"418");
        if (hardware && (adreno418 || otherHardware) && !app.adapter) {
            app.adapter = candidate;
            app.description = description;
            app.adreno418 = adreno418;
        } else { release(candidate); }
    }
    if (!app.adapter) {
        fprintf(app.log, "No hardware Adreno 418 adapter. WARP, reference and software fallbacks are forbidden.\n");
        return false;
    }
    return true;
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM first, LPARAM second)
{
    if (message == WM_CLOSE || (message == WM_KEYDOWN && first == VK_ESCAPE) ||
        (message == WM_LBUTTONUP && GetTickCount64() - windowStarted > 2000)) {
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, first, second);
}

bool createWindow(App &app, HINSTANCE instance)
{
    SetProcessDPIAware();
    WNDCLASSW kind = {};
    kind.lpfnWndProc = windowProcedure;
    kind.hInstance = instance;
    kind.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    kind.lpszClassName = L"LumiaAdrenoHardwareDemo";
    if (!RegisterClassW(&kind)) { return false; }
    app.width = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    app.height = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
    if (app.width < 320 || app.height < 240) { return false; }
    app.window = CreateWindowExW(0, kind.lpszClassName, L"Adreno 418 hardware animation - not iOS",
        WS_POPUP, 0, 0, static_cast<int>(app.width), static_cast<int>(app.height),
        nullptr, nullptr, instance, nullptr);
    return app.window != nullptr;
}

bool target(App &app)
{
    ID3D11Texture2D *buffer = nullptr;
    if (!succeeded(app, app.swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(&buffer)), "GetBuffer")) { return false; }
    D3D11_TEXTURE2D_DESC description = {};
    buffer->GetDesc(&description);
    app.width = description.Width;
    app.height = description.Height;
    const HRESULT result = app.device->CreateRenderTargetView(buffer, nullptr, &app.target);
    release(buffer);
    return succeeded(app, result, "CreateRenderTargetView");
}

bool createGraphics(App &app)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected = {};
    if (!succeeded(app, D3D11CreateDevice(app.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        levels, sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION,
        &app.device, &selected, &app.context), "D3D11CreateDevice(explicit hardware adapter)")) { return false; }
    fprintf(app.log, "FeatureLevel=0x%x\nCPU/software device fallback=false\n", selected);
    IDXGIFactory2 *factory2 = nullptr;
    HRESULT swapped = E_NOINTERFACE;
    if (SUCCEEDED(app.factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory2)))) {
        DXGI_SWAP_CHAIN_DESC1 description = {};
        description.Width = app.width;
        description.Height = app.height;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        IDXGISwapChain1 *created = nullptr;
        swapped = factory2->CreateSwapChainForHwnd(app.device, app.window, &description, nullptr, nullptr, &created);
        if (SUCCEEDED(swapped)) { app.swap = created; }
        release(factory2);
    }
    if (FAILED(swapped)) {
        fprintf(app.log, "Flip sequential unsupported (0x%08lx); trying hardware blt presentation.\n",
            static_cast<unsigned long>(swapped));
        DXGI_SWAP_CHAIN_DESC description = {};
        description.BufferDesc.Width = app.width;
        description.BufferDesc.Height = app.height;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.OutputWindow = app.window;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapped = app.factory->CreateSwapChain(app.device, &description, &app.swap);
        fprintf(app.log, "Presentation=hardware blt; display statistics may be unavailable\n");
    } else { fprintf(app.log, "Presentation=flip sequential\n"); }
    if (!succeeded(app, swapped, "CreateSwapChain")) { return false; }
    app.factory->MakeWindowAssociation(app.window, DXGI_MWA_NO_ALT_ENTER);
    if (!target(app) ||
        !succeeded(app, app.device->CreateVertexShader(app.vertexCode->GetBufferPointer(),
            app.vertexCode->GetBufferSize(), nullptr, &app.vertex), "CreateVertexShader") ||
        !succeeded(app, app.device->CreatePixelShader(app.pixelCode->GetBufferPointer(),
            app.pixelCode->GetBufferSize(), nullptr, &app.pixel), "CreatePixelShader")) { return false; }
    D3D11_BUFFER_DESC uniform = {};
    uniform.ByteWidth = 16;
    uniform.Usage = D3D11_USAGE_DEFAULT;
    uniform.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!succeeded(app, app.device->CreateBuffer(&uniform, nullptr, &app.uniforms), "CreateBuffer")) { return false; }
    for (Timestamp &query : app.queries) {
        D3D11_QUERY_DESC description = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (!succeeded(app, app.device->CreateQuery(&description, &query.disjoint), "CreateQuery(disjoint)")) { return false; }
        description.Query = D3D11_QUERY_TIMESTAMP;
        if (!succeeded(app, app.device->CreateQuery(&description, &query.begin), "CreateQuery(begin)") ||
            !succeeded(app, app.device->CreateQuery(&description, &query.end), "CreateQuery(end)")) { return false; }
    }
    return true;
}

bool createHud(App &app)
{
    app.hudDc = CreateCompatibleDC(nullptr);
    if (!app.hudDc) { return false; }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kHudWidth;
    info.bmiHeader.biHeight = -static_cast<LONG>(kHudHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    app.hudBitmap = CreateDIBSection(app.hudDc, &info, DIB_RGB_COLORS,
        reinterpret_cast<void **>(&app.hudPixels), nullptr, 0);
    app.hudFont = CreateFontW(-27, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
    if (!app.hudBitmap || !app.hudPixels || !app.hudFont) { return false; }
    app.oldBitmap = SelectObject(app.hudDc, app.hudBitmap);
    app.oldFont = SelectObject(app.hudDc, app.hudFont);
    SetBkMode(app.hudDc, TRANSPARENT);
    SetTextColor(app.hudDc, RGB(255, 255, 255));
    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = kHudWidth;
    texture.Height = kHudHeight;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SAMPLER_DESC sampling = {};
    sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampling.MaxLOD = D3D11_FLOAT32_MAX;
    return succeeded(app, app.device->CreateTexture2D(&texture, nullptr, &app.hud), "CreateTexture2D(HUD)") &&
        succeeded(app, app.device->CreateShaderResourceView(app.hud, nullptr, &app.hudView), "CreateShaderResourceView") &&
        succeeded(app, app.device->CreateSamplerState(&sampling, &app.sampler), "CreateSamplerState");
}

void hud(App &app, double submittedRate, unsigned remaining)
{
    GdiFlush();
    ZeroMemory(app.hudPixels, kHudWidth * kHudHeight * sizeof(uint32_t));
    wchar_t line[160];
    const wchar_t *heading = app.adreno418 ?
        L"ADRENO 418 | HARDWARE D3D11" : L"OTHER HARDWARE | NOT A LUMIA RESULT";
    TextOutW(app.hudDc, 16, 10, heading, static_cast<int>(wcslen(heading)));
    const wchar_t *baseline = L"Windows hardware baseline - NOT iOS/XNU";
    TextOutW(app.hudDc, 16, 44, baseline, static_cast<int>(wcslen(baseline)));
    if (app.display.available) {
        swprintf(line, 160, L"DXGI display %.1f FPS | refresh %.1f Hz",
            app.display.framesPerSecond, app.display.refreshesPerSecond);
    } else { swprintf(line, 160, L"DXGI display FPS: unavailable / measuring"); }
    TextOutW(app.hudDc, 16, 88, line, static_cast<int>(wcslen(line)));
    swprintf(line, 160, L"Present loop %.1f FPS (not display FPS)", submittedRate);
    TextOutW(app.hudDc, 16, 122, line, static_cast<int>(wcslen(line)));
    if (app.gpuSamples) { swprintf(line, 160, L"GPU %.2f ms | hardware timestamps verified", app.gpuMilliseconds); }
    else { swprintf(line, 160, L"GPU timing: waiting for a valid hardware query"); }
    TextOutW(app.hudDc, 16, 156, line, static_cast<int>(wcslen(line)));
    swprintf(line, 160, L"%ux%u | VSync=1 | tap to exit | %u s left", app.width, app.height, remaining);
    TextOutW(app.hudDc, 16, 200, line, static_cast<int>(wcslen(line)));
    GdiFlush();
    for (unsigned index = 0; index < kHudWidth * kHudHeight; ++index) {
        const uint32_t pixel = app.hudPixels[index];
        const uint32_t coverage = pixel & 255;
        app.hudPixels[index] = 0x00ffffffu | (coverage << 24);
    }
    app.context->UpdateSubresource(app.hud, 0, nullptr, app.hudPixels, kHudWidth * 4, 0);
}

bool collectQueries(App &app)
{
    for (unsigned attempt = 0; attempt < kQueries; ++attempt) {
        Timestamp &query = app.queries[app.pendingQuery];
        if (!query.pending) { break; }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        UINT64 begin = 0, end = 0;
        HRESULT result = app.context->GetData(query.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) { break; }
        if (!succeeded(app, result, "GetData(disjoint)")) { return false; }
        result = app.context->GetData(query.begin, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) { break; }
        if (!succeeded(app, result, "GetData(begin)")) { return false; }
        result = app.context->GetData(query.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) { break; }
        if (!succeeded(app, result, "GetData(end)")) { return false; }
        double duration = 0;
        if (lumia_demo::validGpuDuration(begin, end, disjoint.Frequency, disjoint.Disjoint != FALSE, duration)) {
            app.gpuMilliseconds = duration;
            ++app.gpuSamples;
        }
        query.pending = false;
        app.pendingQuery = (app.pendingQuery + 1) % kQueries;
    }
    return true;
}

bool screenshot(App &app)
{
    ID3D11Texture2D *buffer = nullptr, *copy = nullptr;
    if (!succeeded(app, app.swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void **>(&buffer)), "Screenshot GetBuffer")) { return false; }
    D3D11_TEXTURE2D_DESC description = {};
    buffer->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    HRESULT result = app.device->CreateTexture2D(&description, nullptr, &copy);
    if (FAILED(result)) { release(buffer); return succeeded(app, result, "Screenshot staging"); }
    app.context->CopyResource(copy, buffer);
    release(buffer);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    result = app.context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) { release(copy); return succeeded(app, result, "Screenshot Map"); }
    wchar_t path[MAX_PATH];
    FILE *file = nullptr;
    if (description.Width <= 16384 && description.Height <= 16384 &&
        swprintf(path, MAX_PATH, L"%ls\\gpu-frame.bmp", app.directory) >= 0) { file = _wfopen(path, L"wb"); }
    bool saved = false;
    if (file) {
        BITMAPFILEHEADER header = {};
        BITMAPINFOHEADER info = {};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + description.Width * description.Height * 4;
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(description.Width);
        info.biHeight = -static_cast<LONG>(description.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        saved = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
            fwrite(&info, 1, sizeof(info), file) == sizeof(info);
        uint8_t *row = static_cast<uint8_t *>(HeapAlloc(GetProcessHeap(), 0, description.Width * 4));
        if (!row) { saved = false; }
        for (UINT vertical = 0; saved && vertical < description.Height; ++vertical) {
            const uint8_t *source = static_cast<const uint8_t *>(mapped.pData) + static_cast<size_t>(vertical) * mapped.RowPitch;
            for (UINT horizontal = 0; horizontal < description.Width; ++horizontal) {
                row[horizontal * 4] = source[horizontal * 4 + 2];
                row[horizontal * 4 + 1] = source[horizontal * 4 + 1];
                row[horizontal * 4 + 2] = source[horizontal * 4];
                row[horizontal * 4 + 3] = 255;
            }
            saved = fwrite(row, 4, description.Width, file) == description.Width;
        }
        if (row) { HeapFree(GetProcessHeap(), 0, row); }
        if (fclose(file) != 0) { saved = false; }
    }
    app.context->Unmap(copy, 0);
    release(copy);
    fprintf(app.log, "ScreenshotSaved=%s\n", saved ? "true" : "false");
    return saved;
}

bool animate(App &app, unsigned seconds)
{
    LARGE_INTEGER frequency = {};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) { return false; }
    const uint64_t ticks = static_cast<uint64_t>(frequency.QuadPart);
    const uint64_t start = counter();
    uint64_t lastHud = start, lastCsv = start, lastFrames = 0, frames = 0;
    double submittedRate = 0;
    bool done = false, captured = false;
    windowStarted = GetTickCount64();
    ShowWindow(app.window, SW_SHOW);
    SetForegroundWindow(app.window);
    hud(app, 0, seconds);
    fprintf(app.log, "Resolution=%ux%u\nSyncInterval=1\nQpcFrequency=%llu\nDurationSeconds=%u\n",
        app.width, app.height, static_cast<unsigned long long>(ticks), seconds);
    while (!done) {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { done = true; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        const uint64_t now = counter();
        const double elapsed = static_cast<double>(now - start) / static_cast<double>(ticks);
        if (done || elapsed >= seconds) { break; }
        if (IsIconic(app.window) || GetForegroundWindow() != app.window) {
            lumia_demo::resetDisplayRate(app.display);
            lastHud = now; lastFrames = frames;
            Sleep(20); continue;
        }
        RECT client = {};
        if (!GetClientRect(app.window, &client)) { return false; }
        if (client.right <= 0 || client.bottom <= 0) { Sleep(20); continue; }
        if (static_cast<UINT>(client.right) != app.width || static_cast<UINT>(client.bottom) != app.height) {
            app.context->OMSetRenderTargets(0, nullptr, nullptr);
            release(app.target);
            if (!succeeded(app, app.swap->ResizeBuffers(0, static_cast<UINT>(client.right),
                static_cast<UINT>(client.bottom), DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers") || !target(app)) { return false; }
            lumia_demo::resetDisplayRate(app.display);
            fprintf(app.log, "Resized=%ux%u\n", app.width, app.height);
        }
        if (!collectQueries(app)) { return false; }
        if (now - lastHud >= ticks) {
            submittedRate = static_cast<double>(frames - lastFrames) * static_cast<double>(ticks) /
                static_cast<double>(now - lastHud);
            hud(app, submittedRate, seconds - static_cast<unsigned>(elapsed));
            lastHud = now; lastFrames = frames;
        }
        const float frame[4] = {static_cast<float>(app.width), static_cast<float>(app.height), static_cast<float>(elapsed), 0};
        app.context->UpdateSubresource(app.uniforms, 0, nullptr, frame, 0, 0);
        const D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(app.width), static_cast<float>(app.height), 0, 1};
        app.context->RSSetViewports(1, &viewport);
        app.context->OMSetRenderTargets(1, &app.target, nullptr);
        app.context->IASetInputLayout(nullptr);
        app.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        app.context->VSSetShader(app.vertex, nullptr, 0);
        app.context->PSSetShader(app.pixel, nullptr, 0);
        app.context->PSSetConstantBuffers(0, 1, &app.uniforms);
        app.context->PSSetShaderResources(0, 1, &app.hudView);
        app.context->PSSetSamplers(0, 1, &app.sampler);
        Timestamp &query = app.queries[app.nextQuery];
        const bool measured = !query.pending;
        if (measured) { app.context->Begin(query.disjoint); app.context->End(query.begin); }
        app.context->Draw(3, 0);
        if (measured) {
            app.context->End(query.end); app.context->End(query.disjoint);
            query.pending = true;
            app.nextQuery = (app.nextQuery + 1) % kQueries;
        }
        if (!captured && elapsed >= 4) { screenshot(app); captured = true; }
        const HRESULT presented = app.swap->Present(1, 0);
        if (!succeeded(app, presented, "Present")) { return false; }
        if (presented != S_OK) {
            lumia_demo::resetDisplayRate(app.display); Sleep(20); continue;
        }
        ++frames;
        DXGI_FRAME_STATISTICS statistics = {};
        const HRESULT sampled = app.swap->GetFrameStatistics(&statistics);
        if (SUCCEEDED(sampled)) {
            const lumia_demo::DisplaySample sample = {statistics.PresentCount, statistics.PresentRefreshCount,
                statistics.SyncRefreshCount, static_cast<uint64_t>(statistics.SyncQPCTime.QuadPart)};
            lumia_demo::updateDisplayRate(app.display, sample, ticks);
        } else { lumia_demo::resetDisplayRate(app.display); }
        if (now - lastCsv >= ticks / 10) {
            fprintf(app.csv, "%llu,%llu,0x%08lx,0x%08lx,%u,%u,%u,%lld,%.6f,%.6f,%.6f,%llu\n",
                static_cast<unsigned long long>(counter()), static_cast<unsigned long long>(frames),
                static_cast<unsigned long>(presented), static_cast<unsigned long>(sampled), statistics.PresentCount,
                statistics.PresentRefreshCount, statistics.SyncRefreshCount, static_cast<long long>(statistics.SyncQPCTime.QuadPart),
                app.display.available ? app.display.framesPerSecond : -1,
                app.display.available ? app.display.refreshesPerSecond : -1,
                app.gpuSamples ? app.gpuMilliseconds : -1, static_cast<unsigned long long>(app.gpuSamples));
            lastCsv = now;
        }
    }
    fprintf(app.log, "SuccessfulPresentCalls=%llu\nValidGpuTimestampSamples=%llu\nDisplayRateWindows=%u\n",
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(app.gpuSamples), app.display.windows);
    if (app.display.available) { fprintf(app.log, "LastDxgiDisplayFps=%.6f\nLastRefreshHz=%.6f\n",
        app.display.framesPerSecond, app.display.refreshesPerSecond); }
    else { fprintf(app.log, "LastDxgiDisplayFps=unavailable\n"); }
    fprintf(app.log, "A requested sync interval or fast Present loop alone is NOT proof of displayed 60 FPS.\n");
    return frames > 0 && app.gpuSamples > 0;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    App app = {};
    if (!openReports(app)) { MessageBoxW(nullptr, L"Could not create demo reports beside the EXE or in TEMP.", L"GPU demo", MB_OK); return 1; }
    int arguments = 0;
    LPWSTR *values = CommandLineToArgvW(GetCommandLineW(), &arguments);
    bool otherHardware = false, shaderCheck = false;
    unsigned seconds = 30;
    bool valid = values != nullptr;
    for (int index = 1; valid && index < arguments; ++index) {
        if (!wcscmp(values[index], L"--allow-other-hardware")) { otherHardware = true; }
        else if (!wcscmp(values[index], L"--shader-check")) { shaderCheck = true; }
        else if (!wcsncmp(values[index], L"--seconds=", 10)) {
            wchar_t *end = nullptr;
            const unsigned long parsed = wcstoul(values[index] + 10, &end, 10);
            valid = end && !*end && parsed >= 5 && parsed <= 300;
            seconds = static_cast<unsigned>(parsed);
        } else { valid = false; }
    }
    if (values) { LocalFree(values); }
    bool passed = valid && compile(app, "vertexMain", "vs_4_0", &app.vertexCode) &&
        compile(app, "pixelMain", "ps_4_0", &app.pixelCode);
    if (passed && !shaderCheck) {
        passed = selectAdapter(app, otherHardware) && createWindow(app, instance) &&
            createGraphics(app) && createHud(app) && animate(app, seconds);
    }
    fprintf(app.log, "Result=%s\nShaderCheckOnly=%s\nOtherHardwareAllowed=%s\n",
        passed ? "PASS" : "FAIL", shaderCheck ? "true" : "false", otherHardware ? "true" : "false");
    if (app.window) { ShowWindow(app.window, SW_HIDE); }
    if (!shaderCheck) {
        wchar_t result[512];
        swprintf(result, 512, L"%ls\n\nReports and GPU screenshot:\n%ls\n\nThis is a Windows hardware baseline, not iOS graphics support.",
            passed ? L"Hardware animation completed." : L"Demo stopped. Read report.txt for the exact failure.", app.directory);
        MessageBoxW(nullptr, result, L"Lumia GPU demo", MB_OK | (passed ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    return passed ? 0 : 1;
}
