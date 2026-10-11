// 一个"自带 AppUserModelID"的窗口探针 —— 用来复现「固定之后出现两个图标」。
//
// 为什么需要它：ZDock 的窗口跟踪器**按 AUMID 优先分组**
//   （WindowTracker.cpp:269 `if (!aumid.empty()) return L"aumid:" + aumid;`）。
//   Chrome / Edge 从快捷方式启动时窗口都会带上 AUMID（快捷方式里就写着），
//   于是它们的分组键是 "aumid:..." 而不是 "path:<exe路径>"。
//   而 DockWin 找分组用的是 `findByExePath()`（只认 "path:" 前缀），永远找不到 → 固定项
//   认不出自己的运行分组。用真 Chrome 复现会打扰用户的浏览器会话，所以造一个等价的窗口。
//
// ⚠ 自检必须用**和 ZDock 完全相同的 API 路径**读回来（SHGetPropertyStoreForWindow +
//   System.AppUserModel.ID）—— "我设上了"不等于"它读得到"。
//
// 用法： _probe_aumid_app.exe [--aumid=<串>] [--title=<标题>]
//   起来后往 stdout 打自检行；窗口一直开着，收到 WM_CLOSE 才退。

#include <windows.h>
#include <shellapi.h>   // SHGetPropertyStoreForWindow 在这里（不在 shobjidl 里）
// ⚠ include 顺序照抄 Src/WindowTracker.cpp（那边有硬要求：propkey.h 的
//   DEFINE_PROPERTYKEY 展开要用到 PROPERTYKEY 类型，必须先由 shobjidl.h 带进来）。
#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <cstdio>
#include <string>

static std::wstring aumidOfWindow(HWND hwnd)
{
    std::wstring out;
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) return out;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        out = pv.pwszVal;
    }
    PropVariantClear(&pv);
    store->Release();
    return out;
}

static void setAumidOnWindow(HWND hwnd, const std::wstring& aumid)
{
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) return;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(InitPropVariantFromString(aumid.c_str(), &pv))) {
        store->SetValue(PKEY_AppUserModel_ID, pv);
        store->Commit();
    }
    PropVariantClear(&pv);
    store->Release();
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, w, l);
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring aumid = L"ZDock.Probe.AumidApp";
    std::wstring title = L"ZDock AUMID 探针";
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a.rfind(L"--aumid=", 0) == 0) aumid = a.substr(8);
        else if (a.rfind(L"--title=", 0) == 0) title = a.substr(8);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 1) 进程级 AUMID —— 等价于"从带 AppUserModelID 的快捷方式启动"
    const HRESULT hrProc = SetCurrentProcessExplicitAppUserModelID(aumid.c_str());

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ZDockAumidProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
        120, 120, 560, 320, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("[aumid] CreateWindow 失败 err=%lu\n", GetLastError());
        return 2;
    }

    // 2) 再把这个 AUMID 显式挂到**窗口**的属性仓上（ZDock 的 aumidOf 读的就是它）
    setAumidOnWindow(hwnd, aumid);
    const std::wstring readback = aumidOfWindow(hwnd);

    // ⚠ 输出一律 ASCII：这个 stdout 由 Python 侧解析，混中文会被代码页搞花。
    //   人看的说明放在脚本里印。
    std::printf("[aumid] want          = %ls\n", aumid.c_str());
    std::printf("[aumid] setprocess hr = 0x%08lX\n", (unsigned long)hrProc);
    std::printf("[aumid] readback      = %ls\n", readback.c_str());
    std::printf("[aumid] hwnd=0x%llX pid=%lu\n",
        static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(hwnd)), GetCurrentProcessId());
    std::printf("[aumid] selfcheck = %s\n", readback == aumid ? "OK" : "FAILED");
    std::fflush(stdout);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
