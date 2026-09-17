// zp-trainer - het venster waarmee je dit bedient.
//
// De GUI injecteert de DLL en praat er daarna mee via gedeeld geheugen. Dat
// scheelt een consolevenster en, belangrijker, je ziet live of het werkt: de
// teller met geblokkeerde schade loopt op zodra er op je units geschoten
// wordt. Werkt het niet, dan staat in het logvenster waar het strandde.

#include "../common/shared.h"
#include "../common/inject.h"
#include "../common/target.h"

#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace zp;

// ------------------------------------------------------------------ opzet --

enum : int {
    ID_INJECT = 1001,
    ID_TOGGLE,
    ID_UNHOOK,
    ID_COPYLOG,
    ID_HOTKEY,
    ID_TIMER = 1,
};

struct HotkeyChoice { const wchar_t* label; uint32_t vk; };

// F10 opent in Windows het venstermenu en F12 is standaard Steam's screenshot,
// dus die staan er bewust niet in. Scroll Lock is in spellen vrijwel nooit
// gebonden en daarmee de veiligste keuze; F9 staat bovenaan omdat elk
// toetsenbord die heeft.
static const HotkeyChoice kHotkeys[] = {
    {L"F9",            VK_F9},
    {L"F8",            VK_F8},
    {L"F7",            VK_F7},
    {L"F6",            VK_F6},
    {L"F5",            VK_F5},
    {L"Scroll Lock",   VK_SCROLL},
    {L"Pause",         VK_PAUSE},
    {L"Insert",        VK_INSERT},
    {L"Home",          VK_HOME},
    {L"End",           VK_END},
    {L"Numpad 0",      VK_NUMPAD0},
    {L"Numpad *",      VK_MULTIPLY},
    {L"Geen sneltoets", 0},
};

static HWND g_main, g_lblGame, g_btnInject, g_btnToggle, g_lblBlocked;
static HWND g_cbHotkey, g_lblHotkeyHint, g_log, g_btnUnhook, g_btnCopy, g_lblWarn;
static HFONT g_font;

static DWORD    g_gamePid = 0;       // gevonden spel
static DWORD    g_hookedPid = 0;     // waar we in geinjecteerd hebben
static HANDLE   g_mapping = nullptr;
static Shared*  g_shared = nullptr;
static uint32_t g_hotkeyVk = VK_F9;
static uint32_t g_lastLogLen = 0;

// ------------------------------------------------------------- instellingen --

static std::string iniPath() { return pathNextToExe("zeropatience.ini"); }

static void loadSettings() {
    UINT vk = GetPrivateProfileIntA("trainer", "hotkey", VK_F9, iniPath().c_str());
    g_hotkeyVk = (uint32_t)vk;
}

static void saveSettings() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", g_hotkeyVk);
    WritePrivateProfileStringA("trainer", "hotkey", buf, iniPath().c_str());
}

// -------------------------------------------------------- gedeeld geheugen --

static void detachShared() {
    if (g_shared) { UnmapViewOfFile(g_shared); g_shared = nullptr; }
    if (g_mapping) { CloseHandle(g_mapping); g_mapping = nullptr; }
    g_lastLogLen = 0;
}

// De DLL maakt de mapping, wij openen hem. Andersom zou niet werken: wij
// draaien verhoogd en het spel meestal niet, en een proces met een laag
// integriteitsniveau komt niet bij objecten van een hoger niveau.
static bool attachShared(DWORD pid) {
    detachShared();
    char name[128];
    sharedName(pid, name, sizeof(name));
    g_mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!g_mapping) return false;
    g_shared = (Shared*)MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                      sizeof(Shared));
    if (!g_shared || g_shared->magic != SHARED_MAGIC) {
        detachShared();
        return false;
    }
    g_shared->hotkeyVk = g_hotkeyVk;
    return true;
}

// ------------------------------------------------------------------- spel --

// Bij de EA-uitgave draaien Generals.exe en Game.dat naast elkaar. De eerste
// is een launcher, de tweede bevat de engine, en die is te herkennen aan het
// geheugengebruik: tientallen MB tegenover honderden.
static DWORD findGame(std::string* nameOut) {
    std::vector<std::pair<uint64_t, ProcEntry>> ranked;
    for (const auto& p : listProcesses())
        if (looksLikeGenerals(p.name))
            ranked.push_back({privateCommitBytes(p.pid), p});
    if (ranked.empty()) return 0;
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (nameOut) *nameOut = ranked[0].second.name;
    return ranked[0].second.pid;
}

// ------------------------------------------------------------------- tekst --

static void setText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

static std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

static void appendLogFromShared() {
    if (!g_shared) return;
    uint32_t len = g_shared->logLength;
    if (len <= g_lastLogLen) return;
    std::string chunk(g_shared->log + g_lastLogLen, len - g_lastLogLen);
    g_lastLogLen = len;

    // Het editveld wil CRLF; de DLL schrijft losse newlines.
    std::string crlf;
    for (char c : chunk) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    std::wstring w = widen(crlf.c_str());
    int end = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, end, end);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

// ------------------------------------------------------------- verversing --

static void refresh() {
    std::string gameName;
    DWORD pid = findGame(&gameName);
    g_gamePid = pid;

    if (!pid) {
        setText(g_lblGame, L"Spel niet gevonden. Start Generals of Zero Hour.");
    } else {
        wchar_t buf[256];
        swprintf(buf, 256, L"Gevonden: %s  (pid %lu)",
                 widen(gameName.c_str()).c_str(), (unsigned long)pid);
        setText(g_lblGame, buf);
    }

    // Verbinding kwijt? Bijvoorbeeld doordat het spel is afgesloten.
    if (g_shared && g_hookedPid && g_hookedPid != pid) {
        detachShared();
        g_hookedPid = 0;
    }
    if (!g_shared && pid) attachShared(pid);   // al geinjecteerd van eerder

    const bool connected = g_shared != nullptr;
    const uint32_t state = connected ? g_shared->state : STATE_STARTING;
    const bool ready = connected && state == STATE_READY;

    EnableWindow(g_btnInject, pid != 0 && !connected);
    EnableWindow(g_btnToggle, ready);
    EnableWindow(g_btnUnhook, ready);

    if (connected) {
        appendLogFromShared();
        const bool on = g_shared->enabled != 0;
        setText(g_btnToggle, ready
                ? (on ? L"Onkwetsbaarheid staat AAN" : L"Onkwetsbaarheid staat UIT")
                : L"Onkwetsbaarheid");

        wchar_t buf[128];
        switch (state) {
            case STATE_CHECKING:  swprintf(buf, 128, L"Bezig: thunk controleren"); break;
            case STATE_RESOLVING: swprintf(buf, 128, L"Bezig: offsets bepalen"); break;
            case STATE_FAILED:    swprintf(buf, 128, L"Mislukt, zie het log"); break;
            case STATE_DETACHED:  swprintf(buf, 128, L"Hook verwijderd"); break;
            default:
                swprintf(buf, 128, L"Schade geblokkeerd: %lu",
                         (unsigned long)g_shared->blockedCount);
                break;
        }
        setText(g_lblBlocked, buf);
    } else {
        setText(g_btnToggle, L"Onkwetsbaarheid");
        setText(g_lblBlocked, L"Nog niet gekoppeld");
    }
}

// --------------------------------------------------------------- opbouwen --

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style,
               int x, int y, int w, int h, HWND parent, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void buildUi(HWND w) {
    g_lblGame = mk(L"STATIC", L"", 0, 14, 12, 500, 20, w, 0);

    g_btnInject = mk(L"BUTTON", L"Koppelen aan het spel", BS_DEFPUSHBUTTON,
                     14, 38, 190, 30, w, ID_INJECT);
    mk(L"STATIC", L"Laad eerst een skirmish met een paar eigen units.",
       0, 214, 46, 300, 20, w, 0);

    g_btnToggle = mk(L"BUTTON", L"Onkwetsbaarheid", BS_PUSHBUTTON,
                     14, 84, 230, 34, w, ID_TOGGLE);
    g_lblBlocked = mk(L"STATIC", L"Nog niet gekoppeld", 0, 256, 94, 258, 20, w, 0);

    mk(L"STATIC", L"Sneltoets in het spel:", 0, 14, 134, 130, 20, w, 0);
    g_cbHotkey = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                    150, 130, 130, 240, w, ID_HOTKEY);
    for (const HotkeyChoice& k : kHotkeys)
        SendMessageW(g_cbHotkey, CB_ADDSTRING, 0, (LPARAM)k.label);
    int sel = 0;
    for (int i = 0; i < (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0])); ++i)
        if (kHotkeys[i].vk == g_hotkeyVk) sel = i;
    SendMessageW(g_cbHotkey, CB_SETCURSEL, sel, 0);
    g_lblHotkeyHint = mk(L"STATIC",
        L"F10 en F12 ontbreken met opzet: F10 opent het venstermenu\n"
        L"van Windows en F12 is standaard Steam's screenshot.",
        0, 292, 130, 222, 36, w, 0);

    mk(L"STATIC", L"Log:", 0, 14, 176, 100, 18, w, 0);
    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                            ES_READONLY | ES_AUTOVSCROLL,
                            14, 196, 500, 228, w, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_btnUnhook = mk(L"BUTTON", L"Hook verwijderen", BS_PUSHBUTTON,
                     14, 434, 160, 28, w, ID_UNHOOK);
    g_btnCopy = mk(L"BUTTON", L"Log kopieren", BS_PUSHBUTTON,
                   182, 434, 130, 28, w, ID_COPYLOG);

    g_lblWarn = mk(L"STATIC",
        L"Alleen voor skirmish en campagne. Er zit geen automatische controle "
        L"op netwerkpotjes in, dus zet dit zelf uit als je tegen andere mensen "
        L"speelt.",
        0, 14, 470, 500, 40, w, 0);
}

// ----------------------------------------------------------------- acties --

static void doInject() {
    if (!g_gamePid) return;
    std::string dll = pathNextToExe("zp-freeze.dll");
    if (GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_main,
            L"zp-freeze.dll staat niet naast dit programma.\n\n"
            L"Zet beide bestanden in dezelfde map en probeer opnieuw.",
            L"zeropatience", MB_ICONWARNING | MB_OK);
        return;
    }

    std::string err;
    if (!injectDll(g_gamePid, dll, &err)) {
        MessageBoxW(g_main, widen(err.c_str()).c_str(), L"Koppelen mislukt",
                    MB_ICONERROR | MB_OK);
        return;
    }
    g_hookedPid = g_gamePid;

    // De DLL maakt zijn mapping in een eigen thread, dus even geduld.
    for (int i = 0; i < 40 && !attachShared(g_gamePid); ++i) Sleep(100);
    refresh();
}

static void copyLog() {
    int n = GetWindowTextLengthW(g_log);
    if (n <= 0) return;
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(g_log, &s[0], n + 1);
    s.resize(n);

    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

// -------------------------------------------------------------- venster --

static LRESULT CALLBACK wndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            buildUi(w);
            SetTimer(w, ID_TIMER, 250, nullptr);
            return 0;

        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

        case WM_TIMER:
            if (wp == ID_TIMER) refresh();
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == ID_INJECT) { doInject(); return 0; }
            if (id == ID_TOGGLE && g_shared) {
                g_shared->enabled = g_shared->enabled ? 0 : 1;
                refresh();
                return 0;
            }
            if (id == ID_UNHOOK && g_shared) {
                g_shared->requestUnhook = 1;
                return 0;
            }
            if (id == ID_COPYLOG) { copyLog(); return 0; }
            if (id == ID_HOTKEY && HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_cbHotkey, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0]))) {
                    g_hotkeyVk = kHotkeys[sel].vk;
                    if (g_shared) g_shared->hotkeyVk = g_hotkeyVk;
                    saveSettings();
                }
                return 0;
            }
            return 0;
        }

        case WM_DESTROY:
            // De hook blijft bewust staan als je dit venster sluit: het spel
            // draait door en je wilt niet dat afsluiten je potje verandert.
            detachShared();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    loadSettings();

    // Het systeemlettertype overnemen, zodat het venster niet uit de toon valt.
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"zeropatienceWnd";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT rc = {0, 0, 528, 522};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"zeropatience",
                             (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX),
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, inst, nullptr);
    if (!g_main) return 1;
    ShowWindow(g_main, show);
    refresh();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
