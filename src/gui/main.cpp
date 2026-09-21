// zp-trainer - het venster waarmee je dit bedient.
//
// Compact van opzet: tijdens het spelen wil je alleen weten of het aan staat
// en of het werkt. Het verloop zit achter een knop, en klapt vanzelf uit als
// er iets misgaat, want dan wil je het juist wel zien.
//
// De GUI injecteert de DLL en praat er daarna mee via gedeeld geheugen. De
// teller met geblokkeerde schade is het punt: die loopt op zodra er op je
// units geschoten wordt, dus je ziet dat het werkt in plaats van het te
// moeten aannemen.

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
    ID_PRIMARY = 1001,   // koppelen, en daarna de aan/uit-schakelaar
    ID_DETAILS,
    ID_UNHOOK,
    ID_COPYLOG,
    ID_HOTKEY,
    ID_PROTECT,
    ID_TIMER = 1,
};

enum : int {
    W_CLIENT      = 344,
    H_COMPACT     = 196,
    H_EXPANDED    = 470,
};

struct HotkeyChoice { const wchar_t* label; uint32_t vk; };

// F10 opent in Windows het venstermenu en F12 is standaard Steam's screenshot,
// dus die staan er bewust niet in. Scroll Lock is in spellen vrijwel nooit
// gebonden; F9 staat bovenaan omdat elk toetsenbord die heeft.
static const HotkeyChoice kHotkeys[] = {
    {L"F9",             VK_F9},
    {L"F8",             VK_F8},
    {L"F7",             VK_F7},
    {L"F6",             VK_F6},
    {L"F5",             VK_F5},
    {L"Scroll Lock",    VK_SCROLL},
    {L"Pause",          VK_PAUSE},
    {L"Insert",         VK_INSERT},
    {L"Home",           VK_HOME},
    {L"End",            VK_END},
    {L"Numpad 0",       VK_NUMPAD0},
    {L"Numpad *",       VK_MULTIPLY},
    {L"Geen sneltoets", 0},
};
static const int kHotkeyCount = (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0]));

static HWND g_main, g_lblStatus, g_btnPrimary, g_lblBlocked, g_lblHotkey;
static HWND g_cbHotkey, g_btnDetails, g_btnUnhook, g_btnCopy, g_log, g_lblWarn;
static HWND g_lblProtect, g_cbProtect;
static HFONT g_font, g_fontBig;

static DWORD    g_gamePid = 0;
static HANDLE   g_mapping = nullptr;
static Shared*  g_shared = nullptr;
static uint32_t g_hotkeyVk = VK_F9;
static int32_t  g_protectPlayer = -1;   // -1 = jouw speler, -2 = iedereen
static uint32_t g_lastLogLen = 0;
static bool     g_expanded = false;
static bool     g_autoExpanded = false;   // eenmalig uitklappen bij een fout
static std::wstring g_gameLabel;

// --------------------------------------------------------- welk spel is dit --

// Generals en Zero Hour heten allebei game.dat, dus de procesnaam zegt niets.
// Het installatiepad wel.
static std::wstring gameLabelFor(DWORD pid, const std::string& procName) {
    std::string path = processImagePath(pid);
    std::string low;
    for (char c : path) low += (char)tolower((unsigned char)c);

    if (low.find("zero hour") != std::string::npos ||
        low.find("zerohour") != std::string::npos)
        return L"Zero Hour";
    if (low.find("generals") != std::string::npos)
        return L"Generals";

    // Pad niet leesbaar: terugvallen op de procesnaam in plaats van gokken.
    std::wstring w(procName.begin(), procName.end());
    return w.empty() ? L"onbekend spel" : w;
}

// ------------------------------------------------------------- instellingen --

static std::string iniPath() { return pathNextToExe("zeropatience.ini"); }

static void loadSettings() {
    g_hotkeyVk = (uint32_t)GetPrivateProfileIntA("trainer", "hotkey", VK_F9,
                                                 iniPath().c_str());
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
    g_autoExpanded = false;
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
    g_shared->protectPlayer = g_protectPlayer;
    return true;
}

// ------------------------------------------------------------------- spel --

// Bij de EA-uitgave draaien Generals.exe en game.dat naast elkaar. De eerste
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
    // Korter dan wat we al hadden betekent dat de DLL opnieuw begonnen is.
    // Dan moet het venster mee, anders blijft de oude mislukking staan.
    if (len < g_lastLogLen) {
        g_lastLogLen = 0;
        SetWindowTextW(g_log, L"");
        g_autoExpanded = false;
    }
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

// ------------------------------------------------------------- uitklappen --

static void setExpanded(bool on) {
    g_expanded = on;
    ShowWindow(g_log, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnCopy, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g_lblProtect, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g_cbProtect, on ? SW_SHOW : SW_HIDE);
    setText(g_btnDetails, on ? L"Minder" : L"Details");

    RECT rc = {0, 0, W_CLIENT, on ? H_EXPANDED : H_COMPACT};
    AdjustWindowRect(&rc, (DWORD)GetWindowLongPtrW(g_main, GWL_STYLE), FALSE);
    SetWindowPos(g_main, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

// ------------------------------------------------------------- verversing --

static void refresh() {
    std::string procName;
    DWORD pid = findGame(&procName);

    if (pid != g_gamePid) {
        g_gamePid = pid;
        g_gameLabel = pid ? gameLabelFor(pid, procName) : L"";
    }

    // Verbinding kwijt, bijvoorbeeld doordat het spel is afgesloten.
    if (g_shared && !pid) detachShared();
    if (!g_shared && pid) attachShared(pid);   // mogelijk al eerder gekoppeld

    const bool connected = g_shared != nullptr;
    const uint32_t state = connected ? g_shared->state : STATE_STARTING;
    const bool ready = connected && state == STATE_READY;
    const bool busy = connected && (state == STATE_CHECKING || state == STATE_RESOLVING);

    if (connected) appendLogFromShared();

    // Statusregel.
    wchar_t buf[256];
    if (!pid) {
        setText(g_lblStatus, L"●  Geen spel gevonden");
    } else if (!connected) {
        swprintf(buf, 256, L"●  %s  ·  pid %lu  ·  niet gekoppeld",
                 g_gameLabel.c_str(), (unsigned long)pid);
        setText(g_lblStatus, buf);
    } else if (state == STATE_FAILED) {
        swprintf(buf, 256, L"●  %s  ·  koppelen mislukt", g_gameLabel.c_str());
        setText(g_lblStatus, buf);
    } else if (state == STATE_DETACHED) {
        swprintf(buf, 256, L"●  %s  ·  losgekoppeld", g_gameLabel.c_str());
        setText(g_lblStatus, buf);
    } else if (busy) {
        swprintf(buf, 256, L"●  %s  ·  %s", g_gameLabel.c_str(),
                 state == STATE_CHECKING ? L"thunk controleren" : L"offsets bepalen");
        setText(g_lblStatus, buf);
    } else if (ready) {
        // De stand hoort in de statusregel, niet alleen op de knop. Een knop
        // waar "AAN" op staat is dubbelzinnig: dat leest als een stand en als
        // een opdracht, en wie hem daarom aanklikt zet juist uit.
        swprintf(buf, 256, L"●  %s  ·  onkwetsbaar %s", g_gameLabel.c_str(),
                 g_shared->enabled ? L"AAN" : L"UIT");
        setText(g_lblStatus, buf);
    } else {
        swprintf(buf, 256, L"●  %s  ·  pid %lu", g_gameLabel.c_str(),
                 (unsigned long)pid);
        setText(g_lblStatus, buf);
    }
    InvalidateRect(g_lblStatus, nullptr, TRUE);

    // De hoofdknop wisselt van rol: koppelen, opnieuw proberen, schakelen.
    // Op de knop staat wat er gebeurt als je hem indrukt, nooit hoe het nu
    // staat -- die stand staat hierboven en in de kleur van de stip.
    if (ready) {
        const bool on = g_shared->enabled != 0;
        setText(g_btnPrimary, on ? L"Uitzetten" : L"Aanzetten");
        EnableWindow(g_btnPrimary, TRUE);
    } else if (busy) {
        setText(g_btnPrimary, L"Bezig...");
        EnableWindow(g_btnPrimary, FALSE);
    } else if (connected && (state == STATE_FAILED || state == STATE_DETACHED)) {
        // De DLL zit al in het proces en wacht op een nieuw verzoek. Opnieuw
        // injecteren zou niets doen, dus dit gaat via het gedeelde geheugen.
        setText(g_btnPrimary, L"Opnieuw proberen");
        EnableWindow(g_btnPrimary, TRUE);
    } else {
        setText(g_btnPrimary, L"Koppelen");
        EnableWindow(g_btnPrimary, pid != 0 && !connected);
    }

    // Teller.
    if (ready) {
        swprintf(buf, 256, L"Schade geblokkeerd:  %lu",
                 (unsigned long)g_shared->blockedCount);
        setText(g_lblBlocked, buf);
    } else if (connected && state == STATE_FAILED) {
        setText(g_lblBlocked, L"Zit je in een potje? Probeer het opnieuw");
    } else {
        setText(g_lblBlocked, L"");
    }

    EnableWindow(g_btnUnhook, ready);

    // Bij een fout klapt het venster eenmalig uit: dan wil je het log zien.
    if (connected && state == STATE_FAILED && !g_autoExpanded && !g_expanded) {
        g_autoExpanded = true;
        setExpanded(true);
    }
}

// --------------------------------------------------------------- opbouwen --

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style,
               int x, int y, int w, int h, HWND parent, int id, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

static void buildUi(HWND w) {
    g_lblStatus = mk(L"STATIC", L"", 0, 14, 12, W_CLIENT - 28, 20, w, 0);

    g_btnPrimary = mk(L"BUTTON", L"Koppelen", BS_PUSHBUTTON,
                      14, 42, 140, 44, w, ID_PRIMARY, g_fontBig);

    g_lblHotkey = mk(L"STATIC", L"Sneltoets", 0, 172, 46, 70, 18, w, 0);
    g_cbHotkey = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                    172, 64, 130, 240, w, ID_HOTKEY);
    for (const HotkeyChoice& k : kHotkeys)
        SendMessageW(g_cbHotkey, CB_ADDSTRING, 0, (LPARAM)k.label);
    int sel = 0;
    for (int i = 0; i < kHotkeyCount; ++i)
        if (kHotkeys[i].vk == g_hotkeyVk) sel = i;
    SendMessageW(g_cbHotkey, CB_SETCURSEL, sel, 0);

    g_lblBlocked = mk(L"STATIC", L"", 0, 14, 98, W_CLIENT - 28, 20, w, 0);

    g_btnDetails = mk(L"BUTTON", L"Details", BS_PUSHBUTTON,
                      14, 126, 90, 28, w, ID_DETAILS);
    g_btnUnhook = mk(L"BUTTON", L"Loskoppelen", BS_PUSHBUTTON,
                     112, 126, 110, 28, w, ID_UNHOOK);
    g_btnCopy = mk(L"BUTTON", L"Log kopieren", BS_PUSHBUTTON,
                   230, 126, 100, 28, w, ID_COPYLOG);

    g_lblWarn = mk(L"STATIC",
        L"Alleen voor skirmish en campagne. Zet dit zelf uit in multiplayer.",
        0, 14, 162, W_CLIENT - 28, 18, w, 0);

    // Wie er beschermd wordt. Staat achter Details omdat je het normaal niet
    // nodig hebt: "van jou" wordt afgeleid. Maar een afleiding kan fout zijn,
    // en dan wil je het zelf kunnen zetten in plaats van te wachten.
    g_lblProtect = mk(L"STATIC", L"Beschermen", 0, 14, 192, 76, 18, w, 0);
    g_cbProtect = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                     96, 188, 234, 300, w, ID_PROTECT);
    SendMessageW(g_cbProtect, CB_ADDSTRING, 0, (LPARAM)L"jouw speler (automatisch)");
    // De tekst hierboven blijft staan, maar "automatisch" betekent nu: de
    // enige speler met PLAYER_HUMAN, en pas als die niet eenduidig is de
    // m_local van de engine. Welke het werd staat in het log.
    SendMessageW(g_cbProtect, CB_ADDSTRING, 0, (LPARAM)L"alle spelers (alleen als test)");
    for (int i = 0; i < 16; ++i) {
        wchar_t b[32];
        swprintf(b, 32, L"alleen speler %d", i);
        SendMessageW(g_cbProtect, CB_ADDSTRING, 0, (LPARAM)b);
    }
    SendMessageW(g_cbProtect, CB_SETCURSEL, 0, 0);

    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VSCROLL | ES_MULTILINE |
                            ES_READONLY | ES_AUTOVSCROLL,
                            14, 220, W_CLIENT - 28, 236, w, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);

    setExpanded(false);
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

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            // De stip in de statusregel kleurt mee: dat leest sneller dan
            // tekst, en tijdens het spelen kijk je maar heel even.
            if ((HWND)lp == g_lblStatus) {
                COLORREF c = RGB(112, 112, 112);            // grijs: geen spel
                if (g_shared) {
                    switch (g_shared->state) {
                        case STATE_READY:
                            c = g_shared->enabled ? RGB(0, 140, 60)    // groen
                                                  : RGB(180, 120, 0);  // amber
                            break;
                        case STATE_FAILED:   c = RGB(190, 40, 40); break;
                        case STATE_CHECKING:
                        case STATE_RESOLVING: c = RGB(180, 120, 0); break;
                        default: break;
                    }
                } else if (g_gamePid) {
                    c = RGB(60, 90, 160);                   // blauw: gevonden
                }
                SetTextColor(dc, c);
            } else if ((HWND)lp == g_lblWarn) {
                SetTextColor(dc, RGB(110, 110, 110));
            }
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }

        case WM_TIMER:
            if (wp == ID_TIMER) refresh();
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == ID_PRIMARY) {
                const uint32_t st = g_shared ? g_shared->state : STATE_STARTING;
                if (g_shared && st == STATE_READY) {
                    g_shared->enabled = g_shared->enabled ? 0 : 1;
                } else if (g_shared && (st == STATE_FAILED || st == STATE_DETACHED)) {
                    g_shared->requestRetry = 1;
                } else {
                    doInject();
                }
                refresh();
                return 0;
            }
            if (id == ID_DETAILS) { setExpanded(!g_expanded); return 0; }
            if (id == ID_UNHOOK && g_shared) {
                g_shared->requestUnhook = 1;
                return 0;
            }
            if (id == ID_COPYLOG) { copyLog(); return 0; }
            if (id == ID_PROTECT && HIWORD(wp) == CBN_SELCHANGE) {
                const int sel = (int)SendMessageW(g_cbProtect, CB_GETCURSEL, 0, 0);
                // 0 -> -1 (de lokale speler), 1 -> -2 (iedereen), 2+k -> k.
                if (sel >= 0) {
                    g_protectPlayer = sel == 0 ? -1 : (sel == 1 ? -2 : sel - 2);
                    if (g_shared) g_shared->protectPlayer = g_protectPlayer;
                }
                return 0;
            }
            if (id == ID_HOTKEY && HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_cbHotkey, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < kHotkeyCount) {
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
            // draait door en je wilt niet dat wegklikken je potje verandert.
            detachShared();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    loadSettings();

    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    LOGFONTW big;
    ZeroMemory(&big, sizeof(big));
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        big = ncm.lfMessageFont;
        big.lfHeight = (LONG)(big.lfHeight * 1.5);
        big.lfWeight = FW_SEMIBOLD;
        g_fontBig = CreateFontIndirectW(&big);
    }
    if (!g_fontBig) g_fontBig = g_font;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"zeropatienceWnd";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    RECT rc = {0, 0, W_CLIENT, H_COMPACT};
    AdjustWindowRect(&rc, style, FALSE);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"zeropatience", style,
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
