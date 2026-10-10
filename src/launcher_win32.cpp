#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr COLORREF Background = RGB(12, 18, 29), Card = RGB(23, 33, 48);
constexpr COLORREF Field = RGB(32, 44, 61), Text = RGB(235, 241, 249);
constexpr COLORREF Muted = RGB(155, 174, 195), Accent = RGB(255, 173, 65);
enum { Iso = 101, BrowseIso, Exe, BrowseExe, Resolution, Mode, Display, Aspect, Vsync, Play, Logs, Status };
const wchar_t* resolutions[] = {L"Native / match display", L"1280 x 720  (720p)", L"1920 x 1080  (1080p)",
                                L"2560 x 1440  (1440p)", L"3840 x 2160  (4K UHD)"};
const wchar_t* resolutionArgs[] = {L"native", L"1280x720", L"1920x1080", L"2560x1440", L"3840x2160"};
// Indices are saved in launcher.ini, so new entries go at the end.
const wchar_t* aspects[] = {L"Original 4:3", L"Stretch to fill display", L"Widescreen 16:9",
                            L"Use in-game Aspect option"};
const wchar_t* aspectArgs[] = {L"4:3", L"stretch", L"16:9", L"auto"};
HWND window;
HINSTANCE instance;
HFONT bodyFont, headingFont, smallFont;
HBRUSH backgroundBrush, cardBrush, fieldBrush;
float dpiScale = 1;
fs::path appDir, settingsDir, settingsFile, logFile;
HANDLE gameProcess = nullptr;
std::vector<std::wstring> monitors;
int px(int value) { return static_cast<int>(value * dpiScale + .5f); }

std::wstring env(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    GetEnvironmentVariableW(name, value.data(), size);
    value.resize(size - 1);
    return value;
}

std::wstring fromUtf8(const std::string& value) {
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring textOf(int id) {
    HWND control = GetDlgItem(window, id);
    std::wstring result(GetWindowTextLengthW(control) + 1, L'\0');
    GetWindowTextW(control, result.data(), static_cast<int>(result.size()));
    result.resize(result.size() - 1);
    return result;
}

std::wstring pref(const wchar_t* key, const std::wstring& fallback = L"") {
    std::vector<wchar_t> value(32768);
    GetPrivateProfileStringW(L"Launcher", key, fallback.c_str(), value.data(),
                             static_cast<DWORD>(value.size()), settingsFile.c_str());
    return value.data();
}

int selected(int id) { return static_cast<int>(SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0)); }
int prefIndex(const wchar_t* key, int fallback, int count) {
    int value = GetPrivateProfileIntW(L"Launcher", key, fallback, settingsFile.c_str());
    return value >= 0 && value < count ? value : fallback;
}

void status(const std::wstring& message) { SetDlgItemTextW(window, Status, message.c_str()); }
void error(const std::wstring& message) {
    status(message);
    MessageBoxW(window, message.c_str(), L"OpenStrikers", MB_OK | MB_ICONERROR);
}

bool saveSettings() {
    std::error_code ec;
    fs::create_directories(settingsDir, ec);
    if (ec) { error(L"Could not create the launcher settings folder."); return false; }
    // Seed a UTF-16 INI so paths with international characters round-trip.
    if (!fs::exists(settingsFile)) {
        std::ofstream seed(settingsFile, std::ios::binary);
        seed.write("\xff\xfe", 2);
    }
    const std::pair<const wchar_t*, std::wstring> values[] = {
        {L"Iso", textOf(Iso)}, {L"Executable", textOf(Exe)},
        {L"Resolution", std::to_wstring(selected(Resolution))},
        {L"Mode", std::to_wstring(selected(Mode))}, {L"Display", std::to_wstring(selected(Display))},
        {L"Aspect", std::to_wstring(selected(Aspect))},
        {L"Vsync", SendDlgItemMessageW(window, Vsync, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0"}
    };
    for (const auto& [key, value] : values) {
        if (!WritePrivateProfileStringW(L"Launcher", key, value.c_str(), settingsFile.c_str())) {
            error(L"Could not save launcher settings. Check the folder permissions.");
            return false;
        }
    }
    return true;
}

// Windows argument escaping, including trailing backslashes before a closing quote.
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

void browse(int id) {
    std::vector<wchar_t> path(32768);
    const auto current = textOf(id);
    std::copy(current.begin(), current.end(), path.begin());
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = id == Iso ? L"GameCube disc images (*.iso;*.gcm)\0*.iso;*.gcm\0All files\0*.*\0"
                                   : L"OpenStrikers executable (*.exe)\0*.exe\0";
    dialog.lpstrTitle = id == Iso ? L"Choose your Super Mario Strikers disc image" : L"Choose OpenStrikers";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        SetDlgItemTextW(window, id, path.data());
        status(L"Ready. Your choices are saved when you play or close the launcher.");
    }
}

void play() {
    if (gameProcess) return;
    const fs::path iso(textOf(Iso)), executable(textOf(Exe));
    std::error_code ec;
    if (!fs::is_regular_file(executable, ec)) { error(L"Choose a valid OpenStrikers executable first."); return; }
    if (!fs::is_regular_file(iso, ec)) { error(L"Choose your game ISO or GCM file first."); return; }
    std::ifstream disc(iso, std::ios::binary);
    unsigned char header[32]{};
    disc.read(reinterpret_cast<char*>(header), sizeof(header));
    if (disc.gcount() != sizeof(header) || header[0] != 'G' || header[1] != '4' || header[2] != 'Q' ||
        header[28] != 0xc2 || header[29] != 0x33 || header[30] != 0x9f || header[31] != 0x3d) {
        error(L"This is not a Super Mario Strikers GameCube ISO/GCM. Choose an uncompressed disc image.");
        return;
    }
    if (!saveSettings()) return;
    fs::create_directories(settingsDir / L"logs", ec);
    if (ec) { error(L"Could not create the game log folder."); return; }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t logName[96];
    swprintf(logName, 96, L"game-%04u%02u%02u-%02u%02u%02u-%03u.log", now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    logFile = settingsDir / L"logs" / logName;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE log = CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        error(L"Could not open the game log file.");
        return;
    }
    std::wstring command = quote(executable.wstring()) + L" " + quote(iso.wstring()) +
        L" --resolution " + resolutionArgs[selected(Resolution)] +
        (selected(Mode) == 0 ? L" --fullscreen" : L" --windowed") +
        L" --display " + std::to_wstring(selected(Display)) +
        L" --aspect " + aspectArgs[selected(Aspect)] +
        (SendDlgItemMessageW(window, Vsync, BM_GETCHECK, 0, 0) == BST_CHECKED ? L" --vsync" : L" --no-vsync");
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = log;
    startup.hStdInput = input;
    PROCESS_INFORMATION process{};
    // The argument string never passes through a shell. Diagnostics go to a file.
    BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process);
    DWORD failure = GetLastError();
    CloseHandle(log);
    CloseHandle(input);
    if (!started) { error(L"Could not start the game (Windows error " + std::to_wstring(failure) + L")."); return; }
    CloseHandle(process.hThread);
    gameProcess = process.hProcess;
    EnableWindow(GetDlgItem(window, Play), FALSE);
    status(L"Game running. F11 or Alt+Enter toggles fullscreen.");
    SetTimer(window, 1, 500, nullptr);
    ShowWindow(window, SW_MINIMIZE);
}

HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    HWND result = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, text,
        WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
    return result;
}
void label(const wchar_t* text, int x, int y, int w, int h = 24, HFONT font = nullptr) {
    HWND item = control(L"STATIC", text, SS_LEFT, 0, x, y, w, h);
    if (font) SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}
void combo(int id, int x, int y, int w, const std::vector<std::wstring>& items, int selection) {
    HWND item = control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                        id, x, y, w, 220);
    SendMessageW(item, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(30));
    SendMessageW(item, CB_SETITEMHEIGHT, 0, px(28));
    for (const auto& text : items) SendMessageW(item, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(item, CB_SETCURSEL, selection, 0);
}

std::wstring initialIso() {
    auto iso = env(L"OPENSTRIKERS_ISO");
    if (!iso.empty()) return iso;
    // Import only this project's ISO setting; never execute the .env file.
    std::ifstream config(appDir.parent_path() / L".env");
    std::string line;
    while (std::getline(config, line)) {
        const std::string key = "OPENSTRIKERS_ISO=";
        if (line.rfind(key, 0) != 0) continue;
        std::string value = line.substr(key.size());
        if (!value.empty() && value.back() == '\r') value.pop_back();
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        return fromUtf8(value);
    }
    return {};
}

void createControls() {
    label(L"OPENSTRIKERS", 36, 24, 660, 44, headingFont);
    label(L"Your next match, ready to play.", 38, 74, 640, 26);
    label(L"GAME DISC", 54, 130, 200, 22, smallFont);
    control(L"EDIT", pref(L"Iso", initialIso()).c_str(), WS_TABSTOP | ES_AUTOHSCROLL, Iso, 54, 156, 534, 34);
    control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, BrowseIso, 604, 156, 114, 34);
    label(L"GAME EXECUTABLE", 54, 208, 260, 22, smallFont);
    control(L"EDIT", pref(L"Executable", (appDir / L"openstrikers.exe").wstring()).c_str(),
            WS_TABSTOP | ES_AUTOHSCROLL, Exe, 54, 234, 534, 34);
    control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, BrowseExe, 604, 234, 114, 34);
    label(L"RENDER RESOLUTION", 54, 324, 260, 22, smallFont);
    combo(Resolution, 54, 350, 288, {std::begin(resolutions), std::end(resolutions)}, prefIndex(L"Resolution", 4, 5));
    label(L"WINDOW MODE", 364, 324, 170, 22, smallFont);
    combo(Mode, 364, 350, 158, {L"Fullscreen", L"Windowed"}, prefIndex(L"Mode", 0, 2));
    label(L"DISPLAY", 544, 324, 174, 22, smallFont);
    combo(Display, 544, 350, 174, monitors, prefIndex(L"Display", 0, static_cast<int>(monitors.size())));
    label(L"PICTURE", 54, 410, 280, 22, smallFont);
    combo(Aspect, 54, 436, 288, {std::begin(aspects), std::end(aspects)}, prefIndex(L"Aspect", 0, 4));
    control(L"BUTTON", L"VSync (reduce tearing)", WS_TABSTOP | BS_AUTOCHECKBOX, Vsync, 364, 437, 340, 32);
    SendDlgItemMessageW(window, Vsync, BM_SETCHECK, prefIndex(L"Vsync", 1, 2) ? BST_CHECKED : BST_UNCHECKED, 0);
    label(L"16:9 uses the game's own widescreen mode: a wider view, with the HUD stretched to fit.",
          54, 488, 650, 24, smallFont);
    label(L"F11 / Alt+Enter: toggle fullscreen during play", 38, 544, 680, 24, smallFont);
    control(L"STATIC", L"Choose your disc once. Everything is remembered for next time.", SS_LEFT,
            Status, 38, 578, 680, 40);
    control(L"BUTTON", L"Open logs", WS_TABSTOP | BS_OWNERDRAW, Logs, 38, 632, 154, 44);
    control(L"BUTTON", L"PLAY GAME", WS_TABSTOP | BS_OWNERDRAW, Play, 504, 628, 230, 48);
}

void drawItem(DRAWITEMSTRUCT* item) {
    const bool button = item->CtlType == ODT_BUTTON;
    const bool primary = button && item->CtlID == Play;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    COLORREF fill = primary && !disabled ? Accent : Field;
    if ((item->itemState & ODS_SELECTED) && !primary) fill = RGB(50, 68, 88);
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(item->hDC, &item->rcItem, brush);
    DeleteObject(brush);
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? Muted : (primary ? Background : Text));
    SelectObject(item->hDC, bodyFont);
    std::wstring title;
    if (button) title = textOf(item->CtlID);
    else if (item->itemID != static_cast<UINT>(-1)) {
        LRESULT count = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
        if (count >= 0) {
            title.resize(count + 1);
            SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(title.data()));
        }
    }
    RECT rect = item->rcItem;
    rect.left += px(10);
    rect.right -= px(10);
    DrawTextW(item->hDC, title.c_str(), -1, &rect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (button ? DT_CENTER : DT_LEFT));
    if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -px(3), -px(3)); DrawFocusRect(item->hDC, &rect); }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        window = hwnd;
        createControls();
        return 0;
    case DM_GETDEFID: return MAKELRESULT(Play, DC_HASDEFID);
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lparam)->itemHeight = px(28);
        return TRUE;
    case WM_DRAWITEM: drawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        RECT rect;
        GetWindowRect(reinterpret_cast<HWND>(lparam), &rect);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&rect), 2);
        bool inCard = rect.top >= px(114) && rect.top < px(530);
        SetBkColor(dc, inCard ? Card : Background);
        SetTextColor(dc, reinterpret_cast<HFONT>(SendMessageW(reinterpret_cast<HWND>(lparam), WM_GETFONT, 0, 0))
                            == headingFont ? Text : Muted);
        return reinterpret_cast<LRESULT>(inCard ? cardBrush : backgroundBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wparam), Text);
        SetBkColor(reinterpret_cast<HDC>(wparam), Field);
        return reinterpret_cast<LRESULT>(fieldBrush);
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, backgroundBrush);
        RECT first{px(38), px(114), px(734), px(292)};
        RECT second{px(38), px(308), px(734), px(530)};
        FillRect(dc, &first, cardBrush);
        FillRect(dc, &second, cardBrush);
        RECT accent{px(38), px(102), px(108), px(105)};
        HBRUSH brush = CreateSolidBrush(Accent);
        FillRect(dc, &accent, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) {
            switch (LOWORD(wparam)) {
            case BrowseIso: browse(Iso); break;
            case BrowseExe: browse(Exe); break;
            case Play: play(); break;
            case Logs: {
                std::error_code ec;
                fs::create_directories(settingsDir / L"logs", ec);
                ShellExecuteW(hwnd, L"open", (settingsDir / L"logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            }
        }
        return 0;
    case WM_TIMER:
        if (gameProcess && WaitForSingleObject(gameProcess, 0) == WAIT_OBJECT_0) {
            DWORD code;
            GetExitCodeProcess(gameProcess, &code);
            CloseHandle(gameProcess);
            gameProcess = nullptr;
            KillTimer(hwnd, 1);
            EnableWindow(GetDlgItem(hwnd, Play), TRUE);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            if (code) error(L"The game exited with code " + std::to_wstring(code) +
                            L". Open logs to see what happened.");
            else status(L"Match finished. Ready when you are.");
        }
        return 0;
    case WM_CLOSE:
        if (saveSettings()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (gameProcess) CloseHandle(gameProcess); // Closing the launcher leaves the game running.
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int show) {
    instance = app;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    dpiScale = std::min(GetDpiForSystem() / 96.f, (work.bottom - work.top - 60) / 704.f);
    std::vector<wchar_t> executable(32768);
    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    appDir = fs::path(executable.data()).parent_path();
    auto overrideDir = env(L"OPENSTRIKERS_LAUNCHER_HOME");
    if (!overrideDir.empty()) settingsDir = overrideDir;
    else {
        PWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) return 1;
        settingsDir = fs::path(localAppData) / L"OpenStrikers";
        CoTaskMemFree(localAppData);
    }
    settingsFile = settingsDir / L"launcher.ini";
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        for (int i = 0; displays && i < count; ++i) {
            const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(displays[i]);
            std::wstring name = L"Display " + std::to_wstring(i + 1);
            if (mode) name = std::to_wstring(i + 1) + L": " + std::to_wstring(mode->w) + L" x " + std::to_wstring(mode->h);
            monitors.push_back(name);
        }
        SDL_free(displays);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    if (monitors.empty()) monitors.push_back(L"Default display");
    bodyFont = CreateFontW(-px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    headingFont = CreateFontW(-px(34), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    smallFont = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    backgroundBrush = CreateSolidBrush(Background);
    cardBrush = CreateSolidBrush(Card);
    fieldBrush = CreateSolidBrush(Field);
    WNDCLASSW cls{};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.lpszClassName = L"OpenStrikersLauncher";
    RegisterClassW(&cls);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, px(772), px(704)};
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_CONTROLPARENT);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"OpenStrikers Launcher", style,
        work.left + std::max(0L, (work.right - work.left - width) / 2),
        work.top + std::max(0L, (work.bottom - work.top - height) / 2), width, height,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    ShowWindow(window, show);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DeleteObject(bodyFont);
    DeleteObject(headingFont);
    DeleteObject(smallFont);
    DeleteObject(backgroundBrush);
    DeleteObject(cardBrush);
    DeleteObject(fieldBrush);
    return static_cast<int>(message.wParam);
}
