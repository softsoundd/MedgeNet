#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define IDI_MEDGENET_APP  101
#define IDR_MEDGENET_LOGO 2001

#define IDC_HOST           1001
#define IDC_FESL_PORT      1002
#define IDC_HTTP_PORT      1003
#define IDC_SAVE           1004
#define IDC_SERVER_PRESET  1005
#define IDC_STATUS         1006
#define IDC_SETTINGS       1007

#define WM_STATUS      (WM_APP + 1)

#define LOCAL_HOST      "127.0.0.1"
#define LOCAL_FESL_PORT "18680"
#define LOCAL_HTTP_PORT "80"

#define DISCLOSURE_CLASS "MedgeNetDisclosureHeader"
#define STATUS_CLASS "MedgeNetStatusCard"

#define MAX_HOST_LEN 64
#define WINDOW_WIDTH 480
#define COLLAPSED_WINDOW_HEIGHT 265
#define LOCAL_WINDOW_HEIGHT 370
#define CUSTOM_WINDOW_HEIGHT 460

enum ServerPreset {
    SERVER_PRESET_LOCAL = 0,
    SERVER_PRESET_CUSTOM = 1
};

static char g_configPath[MAX_PATH];
static char g_dllPath[MAX_PATH];
static char g_host[MAX_HOST_LEN] = LOCAL_HOST;
static char g_feslPort[16] = LOCAL_FESL_PORT;
static char g_httpPort[16] = LOCAL_HTTP_PORT;
static HWND g_mainWindow;
static HANDLE g_worker;
static volatile LONG g_waiting;
static HFONT g_uiFont;
static HFONT g_titleFont;
static BOOL g_deleteUiFont;
static BOOL g_deleteTitleFont;
static ULONG_PTR g_gdiplusToken;
static Gdiplus::Image* g_logoImage;
static IStream* g_logoStream;
static BOOL g_disclosureHot;
static HWND g_settingsControls[8];
static int g_settingsControlCount;
static HWND g_customControls[8];
static int g_customControlCount;
static BOOL g_settingsExpanded;
static HWND g_settingsToggle;
static HWND g_serverCombo;
static HWND g_statusControl;
static HWND g_hintControl;
static HWND g_saveButton;
static HWND g_hostEdit;
static HWND g_feslPortEdit;
static HWND g_httpPortEdit;

static void send_settings_toggle(HWND hwnd)
{
    SendMessageA(GetParent(hwnd), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
}

static void draw_disclosure_chevron(HDC dc, int x, int y, BOOL expanded, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = pen ? (HPEN)SelectObject(dc, pen) : NULL;

    if (expanded) {
        MoveToEx(dc, x, y, NULL);
        LineTo(dc, x + 5, y + 5);
        LineTo(dc, x + 10, y);
    } else {
        MoveToEx(dc, x + 2, y - 2, NULL);
        LineTo(dc, x + 7, y + 3);
        LineTo(dc, x + 2, y + 8);
    }

    if (oldPen)
        SelectObject(dc, oldPen);
    if (pen)
        DeleteObject(pen);
}

static LRESULT CALLBACK disclosure_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!g_disclosureHot) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            g_disclosureHot = TRUE;
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_MOUSELEAVE:
        g_disclosureHot = FALSE;
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_LBUTTONUP:
        send_settings_toggle(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            send_settings_toggle(hwnd);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        RECT textRc;
        HBRUSH bg;
        HPEN linePen;
        HPEN oldPen;
        COLORREF textColor = GetSysColor(COLOR_WINDOWTEXT);
        COLORREF accentColor = GetSysColor(g_disclosureHot ? COLOR_HOTLIGHT : COLOR_GRAYTEXT);

        BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        bg = GetSysColorBrush(g_disclosureHot ? COLOR_BTNFACE : COLOR_WINDOW);
        FillRect(ps.hdc, &rc, bg);

        linePen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DLIGHT));
        oldPen = linePen ? (HPEN)SelectObject(ps.hdc, linePen) : NULL;
        MoveToEx(ps.hdc, 0, rc.bottom - 1, NULL);
        LineTo(ps.hdc, rc.right, rc.bottom - 1);
        if (oldPen)
            SelectObject(ps.hdc, oldPen);
        if (linePen)
            DeleteObject(linePen);

        draw_disclosure_chevron(ps.hdc, 20, g_settingsExpanded ? 13 : 11,
                                g_settingsExpanded, accentColor);

        textRc = rc;
        textRc.left = 42;
        textRc.top = 0;
        textRc.bottom -= 1;
        SetBkMode(ps.hdc, TRANSPARENT);
        SetTextColor(ps.hdc, textColor);
        if (g_uiFont)
            SelectObject(ps.hdc, g_uiFont);
        DrawTextA(ps.hdc, "Server settings", -1, &textRc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

        if (GetFocus() == hwnd) {
            RECT focusRc = rc;
            InflateRect(&focusRc, -3, -3);
            DrawFocusRect(ps.hdc, &focusRc);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static BOOL register_disclosure_class(HINSTANCE hInstance)
{
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = disclosure_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = DISCLOSURE_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    return RegisterClassA(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static char ascii_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static BOOL text_contains_i(const char* text, const char* needle)
{
    int textLen = lstrlenA(text);
    int needleLen = lstrlenA(needle);
    int i;
    int j;

    if (!needleLen)
        return TRUE;
    if (needleLen > textLen)
        return FALSE;

    for (i = 0; i <= textLen - needleLen; i++) {
        for (j = 0; j < needleLen; j++) {
            if (ascii_lower(text[i + j]) != ascii_lower(needle[j]))
                break;
        }
        if (j == needleLen)
            return TRUE;
    }

    return FALSE;
}

static COLORREF status_accent_color(const char* text)
{
    if (text_contains_i(text, "failed") || text_contains_i(text, "could not") || text_contains_i(text, "needs administrator"))
        return RGB(196, 43, 28);
    if (text_contains_i(text, "warning"))
        return RGB(202, 138, 4);
    if (text_contains_i(text, "not running") || text_contains_i(text, "stopped"))
        return RGB(128, 128, 128);
    if (text_contains_i(text, "running") || text_contains_i(text, "ready") || text_contains_i(text, "saved"))
        return RGB(16, 124, 65);
    return RGB(237, 28, 36);
}

static LRESULT CALLBACK status_card_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SETTEXT:
    {
        LRESULT result = DefWindowProcA(hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, TRUE);
        return result;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        RECT labelRc;
        RECT textRc;
        char text[256];
        HBRUSH cardBrush;
        HPEN borderPen;
        HPEN oldPen;
        HBRUSH oldBrush;
        HBRUSH dotBrush;
        COLORREF accent;

        BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        GetWindowTextA(hwnd, text, sizeof(text));
        accent = status_accent_color(text);

        FillRect(ps.hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
        cardBrush = CreateSolidBrush(RGB(250, 250, 250));
        borderPen = CreatePen(PS_SOLID, 1, RGB(229, 229, 229));
        oldBrush = cardBrush ? (HBRUSH)SelectObject(ps.hdc, cardBrush) : NULL;
        oldPen = borderPen ? (HPEN)SelectObject(ps.hdc, borderPen) : NULL;
        RoundRect(ps.hdc, 0, 0, rc.right, rc.bottom, 14, 14);
        if (oldPen)
            SelectObject(ps.hdc, oldPen);
        if (oldBrush)
            SelectObject(ps.hdc, oldBrush);
        if (borderPen)
            DeleteObject(borderPen);
        if (cardBrush)
            DeleteObject(cardBrush);

        dotBrush = CreateSolidBrush(accent);
        oldBrush = dotBrush ? (HBRUSH)SelectObject(ps.hdc, dotBrush) : NULL;
        oldPen = (HPEN)SelectObject(ps.hdc, GetStockObject(NULL_PEN));
        Ellipse(ps.hdc, 18, 22, 30, 34);
        SelectObject(ps.hdc, oldPen);
        if (oldBrush)
            SelectObject(ps.hdc, oldBrush);
        if (dotBrush)
            DeleteObject(dotBrush);

        SetBkMode(ps.hdc, TRANSPARENT);
        if (g_uiFont)
            SelectObject(ps.hdc, g_uiFont);

        labelRc.left = 44;
        labelRc.top = 9;
        labelRc.right = rc.right - 16;
        labelRc.bottom = 25;
        SetTextColor(ps.hdc, RGB(96, 96, 96));
        DrawTextA(ps.hdc, "STATUS", -1, &labelRc, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);

        textRc.left = 44;
        textRc.top = 27;
        textRc.right = rc.right - 16;
        textRc.bottom = rc.bottom - 8;
        SetTextColor(ps.hdc, GetSysColor(COLOR_WINDOWTEXT));
        DrawTextA(ps.hdc, text, -1, &textRc, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static BOOL register_status_class(HINSTANCE hInstance)
{
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = status_card_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = STATUS_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    return RegisterClassA(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static BOOL start_gdiplus(void)
{
    Gdiplus::GdiplusStartupInput input;
    return Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, NULL) == Gdiplus::Ok;
}

static BOOL load_logo_image(void)
{
    HRSRC resource = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_MEDGENET_LOGO), RT_RCDATA);
    HGLOBAL loadedResource;
    DWORD size;
    const void* source;
    HGLOBAL buffer;
    void* dest;

    if (!resource || !g_gdiplusToken)
        return FALSE;

    loadedResource = LoadResource(NULL, resource);
    size = SizeofResource(NULL, resource);
    source = loadedResource ? LockResource(loadedResource) : NULL;
    if (!source || !size)
        return FALSE;

    buffer = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!buffer)
        return FALSE;

    dest = GlobalLock(buffer);
    if (!dest) {
        GlobalFree(buffer);
        return FALSE;
    }
    CopyMemory(dest, source, size);
    GlobalUnlock(buffer);

    if (CreateStreamOnHGlobal(buffer, TRUE, &g_logoStream) != S_OK) {
        GlobalFree(buffer);
        return FALSE;
    }

    g_logoImage = Gdiplus::Image::FromStream(g_logoStream);
    if (!g_logoImage || g_logoImage->GetLastStatus() != Gdiplus::Ok) {
        delete g_logoImage;
        g_logoImage = NULL;
        g_logoStream->Release();
        g_logoStream = NULL;
        return FALSE;
    }

    return TRUE;
}

static void draw_header(HDC dc)
{
    RECT textRc;

    if (g_logoImage) {
        const int maxWidth = 420;
        const int maxHeight = 40;
        UINT imageWidth = g_logoImage->GetWidth();
        UINT imageHeight = g_logoImage->GetHeight();
        double scaleX = (double)maxWidth / (double)imageWidth;
        double scaleY = (double)maxHeight / (double)imageHeight;
        double scale = scaleX < scaleY ? scaleX : scaleY;
        INT drawWidth = (INT)(imageWidth * scale);
        INT drawHeight = (INT)(imageHeight * scale);

        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.DrawImage(g_logoImage, Gdiplus::Rect(20, 18, drawWidth, drawHeight));
        return;
    }

    textRc.left = 20;
    textRc.top = 16;
    textRc.right = 440;
    textRc.bottom = 54;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(237, 28, 36));
    if (g_titleFont)
        SelectObject(dc, g_titleFont);
    DrawTextA(dc, "MEDGENET", -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
}

static void stop_gdiplus(void)
{
    delete g_logoImage;
    g_logoImage = NULL;
    if (g_logoStream) {
        g_logoStream->Release();
        g_logoStream = NULL;
    }
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

static HICON load_app_icon(HINSTANCE hInstance, int width, int height)
{
    return (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_MEDGENET_APP),
                             IMAGE_ICON, width, height,
                             LR_DEFAULTCOLOR | LR_SHARED);
}

static void set_window_icons(HWND hwnd, HINSTANCE hInstance)
{
    HICON bigIcon = load_app_icon(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    HICON smallIcon = load_app_icon(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

    if (bigIcon)
        SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);
    if (smallIcon)
        SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
}

static HWND create_control(const char* className, const char* text, DWORD style,
                           int x, int y, int width, int height,
                           HWND parent, HMENU id)
{
    HWND control = CreateWindowA(className, text, style, x, y, width, height, parent, id, NULL, NULL);
    if (control && g_uiFont)
        SendMessageA(control, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    return control;
}

static HWND create_custom_control(const char* className, const char* text, DWORD style,
                                  int x, int y, int width, int height,
                                  HWND parent, HMENU id)
{
    HWND control = create_control(className, text, style & ~WS_VISIBLE, x, y, width, height, parent, id);
    if (control && g_customControlCount < (int)(sizeof(g_customControls) / sizeof(g_customControls[0])))
        g_customControls[g_customControlCount++] = control;
    return control;
}

static HWND create_settings_control(const char* className, const char* text, DWORD style,
                                    int x, int y, int width, int height,
                                    HWND parent, HMENU id)
{
    HWND control = create_control(className, text, style & ~WS_VISIBLE, x, y, width, height, parent, id);
    if (control && g_settingsControlCount < (int)(sizeof(g_settingsControls) / sizeof(g_settingsControls[0])))
        g_settingsControls[g_settingsControlCount++] = control;
    return control;
}

static BOOL is_local_config(void)
{
    return lstrcmpA(g_host, LOCAL_HOST) == 0 &&
           lstrcmpA(g_feslPort, LOCAL_FESL_PORT) == 0 &&
           lstrcmpA(g_httpPort, LOCAL_HTTP_PORT) == 0;
}

static int selected_server_preset(void)
{
    int i;
    if (!g_serverCombo)
        return is_local_config() ? SERVER_PRESET_LOCAL : SERVER_PRESET_CUSTOM;
    i = (int)SendMessageA(g_serverCombo, CB_GETCURSEL, 0, 0);
    return i == SERVER_PRESET_CUSTOM ? SERVER_PRESET_CUSTOM : SERVER_PRESET_LOCAL;
}

static void update_server_layout(HWND hwnd)
{
    int i;
    BOOL custom = g_settingsExpanded && selected_server_preset() == SERVER_PRESET_CUSTOM;

    InvalidateRect(g_settingsToggle, NULL, TRUE);

    for (i = 0; i < g_settingsControlCount; i++)
        ShowWindow(g_settingsControls[i], g_settingsExpanded ? SW_SHOW : SW_HIDE);

    for (i = 0; i < g_customControlCount; i++)
        ShowWindow(g_customControls[i], custom ? SW_SHOW : SW_HIDE);

    if (g_settingsExpanded) {
        SetWindowTextA(g_hintControl, custom ?
            "Use a MedgeNet host/IP and ports supplied by the server operator." :
            "Use a MedgeNet server running on this PC.");
        MoveWindow(g_saveButton, 20, custom ? 383 : 291, 130, 32, TRUE);
    }
    SetWindowPos(hwnd, NULL, 0, 0, WINDOW_WIDTH,
                 g_settingsExpanded ? (custom ? CUSTOM_WINDOW_HEIGHT : LOCAL_WINDOW_HEIGHT) : COLLAPSED_WINDOW_HEIGHT,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void create_ui_fonts(void)
{
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc)
        ReleaseDC(NULL, dc);

    g_uiFont = CreateFontA(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    if (g_uiFont)
        g_deleteUiFont = TRUE;
    else
        g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_titleFont = CreateFontA(-MulDiv(16, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    if (g_titleFont)
        g_deleteTitleFont = TRUE;
    else
        g_titleFont = g_uiFont;
}

static void set_status_async(const char* text)
{
    char* copy = (char*)HeapAlloc(GetProcessHeap(), 0, lstrlenA(text) + 1);
    if (!copy) return;
    lstrcpyA(copy, text);
    PostMessageA(g_mainWindow, WM_STATUS, 0, (LPARAM)copy);
}

static void build_paths(void)
{
    char exe[MAX_PATH];
    char* slash = exe;
    char* p;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    for (p = exe; *p; p++) {
        if (*p == '\\' || *p == '/') slash = p;
    }
    slash[1] = '\0';
    lstrcpyA(g_configPath, exe);
    lstrcatA(g_configPath, "MedgeNetClient.ini");
    lstrcpyA(g_dllPath, exe);
    lstrcatA(g_dllPath, "MedgeNetClient.dll");
}

static void load_config(void)
{
    GetPrivateProfileStringA("Server", "Host", LOCAL_HOST, g_host, sizeof(g_host), g_configPath);
    GetPrivateProfileStringA("Server", "Port", LOCAL_FESL_PORT, g_feslPort, sizeof(g_feslPort), g_configPath);
    GetPrivateProfileStringA("Server", "HTTPPort", LOCAL_HTTP_PORT, g_httpPort, sizeof(g_httpPort), g_configPath);
}

static void save_config(HWND hwnd)
{
    if (selected_server_preset() == SERVER_PRESET_LOCAL) {
        lstrcpyA(g_host, LOCAL_HOST);
        lstrcpyA(g_feslPort, LOCAL_FESL_PORT);
        lstrcpyA(g_httpPort, LOCAL_HTTP_PORT);
    } else {
        GetDlgItemTextA(hwnd, IDC_HOST, g_host, sizeof(g_host));
        GetDlgItemTextA(hwnd, IDC_FESL_PORT, g_feslPort, sizeof(g_feslPort));
        GetDlgItemTextA(hwnd, IDC_HTTP_PORT, g_httpPort, sizeof(g_httpPort));
    }
    WritePrivateProfileStringA("Server", "Host", g_host, g_configPath);
    WritePrivateProfileStringA("Server", "Port", g_feslPort, g_configPath);
    WritePrivateProfileStringA("Server", "HTTPPort", g_httpPort, g_configPath);
    SetDlgItemTextA(hwnd, IDC_STATUS, selected_server_preset() == SERVER_PRESET_LOCAL ?
                    "Saved local server configuration." :
                    "Saved custom server configuration.");
}

static BOOL adjust_debug_privilege(void)
{
    HANDLE token;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;
    if (!LookupPrivilegeValueA(NULL, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return FALSE;
    }
    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

static DWORD find_process(const char* name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry;
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (lstrcmpiA(entry.szExeFile, name) == 0) {
                DWORD pid = entry.th32ProcessID;
                CloseHandle(snapshot);
                return pid;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

static BOOL has_module(DWORD pid, const char* moduleName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32 entry;
    if (snapshot == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry)) {
        do {
            if (lstrcmpiA(entry.szModule, moduleName) == 0) {
                CloseHandle(snapshot);
                return TRUE;
            }
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return FALSE;
}

static BOOL process_ready(DWORD pid)
{
    return has_module(pid, "openal32.dll") || has_module(pid, "d3d9.dll");
}

static BOOL inject_dll(HANDLE process)
{
    SIZE_T size = lstrlenA(g_dllPath) + 1;
    void* remote = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE thread;
    HMODULE kernel32;
    FARPROC loadLibrary;
    if (!remote) return FALSE;
    if (!WriteProcessMemory(process, remote, g_dllPath, size, NULL)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return FALSE;
    }
    kernel32 = GetModuleHandleA("kernel32.dll");
    loadLibrary = GetProcAddress(kernel32, "LoadLibraryA");
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibrary, remote, 0, NULL);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return FALSE;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return TRUE;
}

static void wait_for_process_exit(HANDLE process)
{
    set_status_async("MedgeNet is running in-game.");
    while (InterlockedCompareExchange(&g_waiting, 1, 1)) {
        DWORD waitResult = WaitForSingleObject(process, 500);
        if (waitResult == WAIT_OBJECT_0)
            return;
        if (waitResult != WAIT_TIMEOUT)
            return;
    }
}

static DWORD WINAPI injector_thread(LPVOID)
{
    if (!adjust_debug_privilege())
        set_status_async("Warning: administrator access is limited. Trying anyway...");

    set_status_async("Game is not running.");
    while (InterlockedCompareExchange(&g_waiting, 1, 1)) {
        DWORD pid = find_process("MirrorsEdge.exe");
        HANDLE process;
        if (!pid) {
            Sleep(500);
            continue;
        }
        process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!process) {
            set_status_async("Game detected, but MedgeNet needs administrator access.");
            Sleep(1000);
            continue;
        }
        if (has_module(pid, "MedgeNetClient.dll")) {
            set_status_async("MedgeNet is already running in-game.");
            wait_for_process_exit(process);
            CloseHandle(process);
            if (InterlockedCompareExchange(&g_waiting, 1, 1))
                set_status_async("Game is not running.");
            Sleep(500);
            continue;
        }
        if (!process_ready(pid)) {
            CloseHandle(process);
            set_status_async("Game detected. Preparing MedgeNet...");
            Sleep(500);
            continue;
        }
        if (inject_dll(process)) {
            set_status_async("MedgeNet is ready.");
            wait_for_process_exit(process);
            CloseHandle(process);
            if (InterlockedCompareExchange(&g_waiting, 1, 1))
                set_status_async("Game is not running.");
            Sleep(500);
            continue;
        }
        CloseHandle(process);
        set_status_async("Could not start MedgeNet in-game. Retrying...");
        Sleep(1000);
    }
    set_status_async("Stopped.");
    return 0;
}

static void start_waiting(HWND hwnd)
{
    if (InterlockedCompareExchange(&g_waiting, 1, 0) != 0)
        return;

    save_config(hwnd);
    SetDlgItemTextA(hwnd, IDC_STATUS, "Game is not running.");
    g_worker = CreateThread(NULL, 0, injector_thread, NULL, 0, NULL);
    if (!g_worker) {
        InterlockedExchange(&g_waiting, 0);
        SetDlgItemTextA(hwnd, IDC_STATUS, "Could not start MedgeNet.");
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        g_statusControl = create_control(STATUS_CLASS, "Game is not running.", WS_CHILD | WS_VISIBLE,
                                         20, 88, 402, 56, hwnd, (HMENU)IDC_STATUS);

        g_settingsToggle = create_control(DISCLOSURE_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          20, 172, 402, 32, hwnd, (HMENU)IDC_SETTINGS);

        create_settings_control("STATIC", "Server", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                20, 222, 92, 20, hwnd, NULL);
        g_serverCombo = create_settings_control("COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                                CBS_DROPDOWNLIST | WS_VSCROLL,
                                                132, 218, 290, 180, hwnd, (HMENU)IDC_SERVER_PRESET);
        SendMessageA(g_serverCombo, CB_ADDSTRING, 0, (LPARAM)"Local server (this PC)");
        SendMessageA(g_serverCombo, CB_ADDSTRING, 0, (LPARAM)"Custom server");
        SendMessageA(g_serverCombo, CB_SETCURSEL,
                     is_local_config() ? SERVER_PRESET_LOCAL : SERVER_PRESET_CUSTOM, 0);

        g_hintControl = create_settings_control("STATIC", "", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                                132, 250, 310, 36, hwnd, NULL);

        create_custom_control("STATIC", "Host / IP", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                              20, 296, 92, 20, hwnd, NULL);
        g_hostEdit = create_custom_control("EDIT", g_host, WS_CHILD | WS_VISIBLE | WS_BORDER |
                                           WS_TABSTOP | ES_AUTOHSCROLL,
                                           132, 292, 290, 24, hwnd, (HMENU)IDC_HOST);
        create_custom_control("STATIC", "FESL port", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                              20, 332, 92, 20, hwnd, NULL);
        g_feslPortEdit = create_custom_control("EDIT", g_feslPort, WS_CHILD | WS_VISIBLE | WS_BORDER |
                                               WS_TABSTOP | ES_AUTOHSCROLL,
                                               132, 328, 86, 24, hwnd, (HMENU)IDC_FESL_PORT);
        create_custom_control("STATIC", "HTTP port", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                              244, 332, 80, 20, hwnd, NULL);
        g_httpPortEdit = create_custom_control("EDIT", g_httpPort, WS_CHILD | WS_VISIBLE | WS_BORDER |
                                               WS_TABSTOP | ES_AUTOHSCROLL,
                                               336, 328, 86, 24, hwnd, (HMENU)IDC_HTTP_PORT);

        g_saveButton = create_settings_control("BUTTON", "Save settings", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               20, 292, 130, 32, hwnd, (HMENU)IDC_SAVE);
        SendMessageA(g_hostEdit, EM_LIMITTEXT, MAX_HOST_LEN - 1, 0);
        SendMessageA(g_feslPortEdit, EM_LIMITTEXT, 5, 0);
        SendMessageA(g_httpPortEdit, EM_LIMITTEXT, 5, 0);
        update_server_layout(hwnd);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SAVE) {
            save_config(hwnd);
        } else if (LOWORD(wParam) == IDC_SERVER_PRESET && HIWORD(wParam) == CBN_SELCHANGE) {
            update_server_layout(hwnd);
        } else if (LOWORD(wParam) == IDC_SETTINGS) {
            g_settingsExpanded = !g_settingsExpanded;
            update_server_layout(hwnd);
        }
        return 0;
    case WM_STATUS:
        SetDlgItemTextA(hwnd, IDC_STATUS, (char*)lParam);
        HeapFree(GetProcessHeap(), 0, (void*)lParam);
        if (!InterlockedCompareExchange(&g_waiting, 0, 0) && g_worker) {
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        FillRect(ps.hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
        draw_header(ps.hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wParam, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        InterlockedExchange(&g_waiting, 0);
        if (g_worker) {
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        if (g_deleteTitleFont && g_titleFont)
            DeleteObject(g_titleFont);
        if (g_deleteUiFont && g_uiFont)
            DeleteObject(g_uiFont);
        stop_gdiplus();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    WNDCLASSA wc;
    MSG msg;

    build_paths();
    load_config();
    create_ui_fonts();
    if (start_gdiplus())
        load_logo_image();
    if (!register_disclosure_class(hInstance)) {
        stop_gdiplus();
        return 1;
    }
    if (!register_status_class(hInstance)) {
        stop_gdiplus();
        return 1;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "MedgeNetLauncherWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = load_app_icon(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    g_mainWindow = CreateWindowA("MedgeNetLauncherWindow", "MedgeNet Launcher",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH, COLLAPSED_WINDOW_HEIGHT,
                                 NULL, NULL, hInstance, NULL);
    set_window_icons(g_mainWindow, hInstance);
    ShowWindow(g_mainWindow, nCmdShow);
    UpdateWindow(g_mainWindow);
    start_waiting(g_mainWindow);

    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
