#include <windows.h>
#include <unknwn.h>
#include <dbghelp.h>
#include <winrt/base.h>
#include <winrt/Windows.UI.Xaml.h>
#include <atomic>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "windowsapp.lib")

using winrt::Windows::UI::Xaml::FrameworkElement;
using winrt::Windows::UI::Xaml::XamlRoot;
using winrt::Windows::UI::Xaml::UIElement;

namespace {
constexpr UINT kApplyMessage = WM_APP + 0x4A1;
constexpr DWORD kShimInit = 1;
constexpr int kTargetBuild = 26300, kTargetUbr = 9711;

void* g_primarySiteVtable{};
void* g_getTaskbarHost{};
void* g_frameHeight{};
void* g_decref{};
std::atomic<bool> g_running{false};
HHOOK g_threadHook{};
HWND g_bridge{};
DWORD g_bridgeThread{};

using GetTaskbarHostFn = void* (__fastcall*)(void*, void**);
using DecrefFn = void (__fastcall*)(void*);

bool SafeReadPointer(const void* address, void** value) {
    __try { *value = *reinterpret_cast<void* const*>(address); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { *value = nullptr; return false; }
}
bool SafeReadBytes(const BYTE* address, BYTE* value, size_t count) {
    __try { for (size_t i=0;i<count;i++) value[i]=address[i]; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsTargetBuild() {
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    auto rtl = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOEXW v{sizeof(v)};
    if (!rtl || rtl(reinterpret_cast<PRTL_OSVERSIONINFOW>(&v)) != 0) return false;
    DWORD ubr = 0;
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD type{}, size=sizeof(ubr);
        RegQueryValueExW(key,L"UBR",nullptr,&type,reinterpret_cast<BYTE*>(&ubr),&size);
        RegCloseKey(key);
    }
    return v.dwBuildNumber == kTargetBuild && ubr == kTargetUbr;
}

struct SymbolMatch { std::wstring needle; void** output; };
BOOL CALLBACK SymbolCallback(PSYMBOL_INFOW symbol, ULONG, PVOID context) {
    auto* matches = static_cast<std::vector<SymbolMatch>*>(context);
    wchar_t demangled[2048]{};
    UnDecorateSymbolNameW(symbol->Name, demangled, ARRAYSIZE(demangled), UNDNAME_COMPLETE);
    const std::wstring name = demangled;
    for (auto& m : *matches) {
        if (!*m.output && name.find(m.needle) != std::wstring::npos) {
            *m.output = reinterpret_cast<void*>(symbol->Address);
        }
    }
    return TRUE;
}

bool ResolveTaskbarSymbols() {
    wchar_t path[MAX_PATH]{};
    if (!GetSystemDirectoryW(path, ARRAYSIZE(path))) return false;
    const std::wstring taskbarPath = std::wstring(path) + L"\\taskbar.dll";
    HANDLE process = GetCurrentProcess();
    if (!SymInitializeW(process, L"srv*C:\\Windows\\Temp\\TaskbarOLED-Symbols*https://msdl.microsoft.com/download/symbols", FALSE) &&
        GetLastError() != ERROR_INVALID_PARAMETER) return false;
    DWORD64 base = SymLoadModuleExW(process, nullptr, taskbarPath.c_str(), nullptr, 0, 0, nullptr, 0);
    if (!base) return false;

    std::vector<SymbolMatch> matches = {
        {L"CTaskBand::", &g_primarySiteVtable},
        {L"CTaskBand::GetTaskbarHost", &g_getTaskbarHost},
        {L"TaskbarHost::FrameHeight", &g_frameHeight},
        {L"std::_Ref_count_base::_Decref", &g_decref}
    };
    if (!SymEnumSymbolsW(process, base, nullptr, SymbolCallback, &matches)) return false;
    // Reject an arbitrary CTaskBand symbol: a vftable must be in read-only image data.
    if (!g_primarySiteVtable || !g_getTaskbarHost || !g_frameHeight || !g_decref) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(g_primarySiteVtable,&mbi,sizeof(mbi)) || !(mbi.Protect & (PAGE_READONLY|PAGE_EXECUTE_READ))) return false;
    return true;
}

void* FindTaskListWndSite(void* taskBand) {
    auto** p = static_cast<void**>(taskBand);
    for (int i=0;i<20;i++,p++) {
        void* value{};
        if (!SafeReadPointer(p,&value)) return nullptr;
        if (value == g_primarySiteVtable) return p;
    }
    return nullptr;
}

UIElement GetTaskbarRootContent(HWND taskbar) {
    if (!taskbar || !g_primarySiteVtable || !g_getTaskbarHost) return nullptr;
    HWND switchWnd = reinterpret_cast<HWND>(GetPropW(taskbar, L"TaskbandHWND"));
    if (!switchWnd) return nullptr;
    void* taskBand = reinterpret_cast<void*>(GetWindowLongPtrW(switchWnd, 0));
    if (!taskBand) return nullptr;
    void* site = FindTaskListWndSite(taskBand);
    if (!site) return nullptr;

    void* shared[2]{};
    reinterpret_cast<GetTaskbarHostFn>(g_getTaskbarHost)(site, shared);
    if (!shared[0]) {
        if (shared[1]) reinterpret_cast<DecrefFn>(g_decref)(shared[1]);
        return nullptr;
    }

    size_t offset = 0x10;
    auto* code = static_cast<const BYTE*>(g_frameHeight);
    BYTE prologue[8]{};
    if (SafeReadBytes(code,prologue,sizeof(prologue)) &&
        prologue[0]==0x48 && prologue[1]==0x83 && prologue[2]==0xEC && prologue[3]==0x28 &&
        prologue[4]==0x48 && prologue[5]==0x83 && prologue[6]==0xC1 && prologue[7]<=0x7f) offset=prologue[7];
    IUnknown* elementUnknown{};
    void* elementPointer{};
    if (SafeReadPointer(static_cast<BYTE*>(shared[0])+offset,&elementPointer))
        elementUnknown = static_cast<IUnknown*>(elementPointer);

    FrameworkElement element{nullptr};
    if (elementUnknown) elementUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));
    XamlRoot root = element ? element.XamlRoot() : nullptr;
    if (shared[1]) reinterpret_cast<DecrefFn>(g_decref)(shared[1]);
    if (!root) return nullptr;
    return root.Content().try_as<UIElement>();
}

bool IsHidden(HWND hwnd) {
    RECT wr{}; if (!GetWindowRect(hwnd,&wr)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi)) return false;
    RECT visible{};
    if (!IntersectRect(&visible,&wr,&mi.rcMonitor)) return true;
    const LONGLONG total = LONGLONG(wr.right-wr.left)*(wr.bottom-wr.top);
    const LONGLONG shown = LONGLONG(visible.right-visible.left)*(visible.bottom-visible.top);
    return total>0 && shown*4<total;
}

LRESULT CALLBACK ThreadHook(int code, WPARAM, LPARAM lp) {
    if (code>=0 && lp) {
        auto* msg=reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (msg->message==kApplyMessage && msg->hwnd==g_bridge) {
            try {
                HWND taskbar=FindWindowW(L"Shell_TrayWnd",nullptr);
                auto content=GetTaskbarRootContent(taskbar);
                if (content) content.Opacity(IsHidden(taskbar) ? 0.0 : 1.0);
            } catch (...) {}
        }
    }
    return CallNextHookEx(g_threadHook,code,0,lp);
}

DWORD WINAPI Worker(void*) {
    if (!IsTargetBuild() || !ResolveTaskbarSymbols()) return 1;
    for (;;) {
        HWND taskbar=FindWindowW(L"Shell_TrayWnd",nullptr);
        HWND bridge=taskbar ? FindWindowExW(taskbar,nullptr,L"Windows.UI.Composition.DesktopWindowContentBridge",nullptr) : nullptr;
        DWORD tid=bridge ? GetWindowThreadProcessId(bridge,nullptr) : 0;
        if (bridge && tid && (bridge!=g_bridge || tid!=g_bridgeThread)) {
            if (g_threadHook) UnhookWindowsHookEx(g_threadHook);
            g_bridge=bridge; g_bridgeThread=tid;
            g_threadHook=SetWindowsHookExW(WH_CALLWNDPROCRET,ThreadHook,GetModuleHandleW(L"AcRes.dll"),tid);
        }
        if (bridge && g_threadHook) PostMessageW(bridge,kApplyMessage,0,0);
        Sleep(150);
    }
    return 0;
}
}

extern "C" __declspec(dllexport) PVOID WINAPI GetHookAPIs(LPSTR, LPWSTR name, PDWORD count) {
    static BYTE emptyHooks[sizeof(void*)]{};
    if (count) *count=0;
    return name && lstrcmpiW(name,L"TaskbarOLED")==0 ? emptyHooks : nullptr;
}
extern "C" __declspec(dllexport) VOID WINAPI NotifyShims(DWORD notification, PVOID) {
    if (notification==kShimInit && !g_running.exchange(true)) {
        HANDLE thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if (thread) CloseHandle(thread);
    }
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
