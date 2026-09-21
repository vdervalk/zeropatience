#include "inject.h"

namespace zp {

std::string pathNextToExe(const char* name) {
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof(self));
    std::string p = self;
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos) p.resize(slash + 1);
    return p + name;
}

bool injectDll(DWORD pid, const std::string& dllPath, std::string* err) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        *err = "Kan het spelproces niet openen (code " +
               std::to_string(GetLastError()) +
               "). Draait dit programma als administrator?";
        return false;
    }

    const SIZE_T bytes = dllPath.size() + 1;
    void* remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        *err = "Geheugen reserveren in het spel mislukte (code " +
               std::to_string(GetLastError()) + ").";
        CloseHandle(h);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(h, remote, dllPath.c_str(), bytes, &written) ||
        written != bytes) {
        *err = "Het pad van de DLL kon niet naar het spel geschreven worden.";
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    FARPROC loadLibrary = GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                         "LoadLibraryA");
    if (!loadLibrary) {
        *err = "LoadLibraryA niet gevonden.";
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    // LoadLibraryA neemt een pointer en geeft een handle terug, dus de vorm
    // past op een threadfunctie. De cast is opzet.
    LPTHREAD_START_ROUTINE start =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(loadLibrary));
    HANDLE th = CreateRemoteThread(h, nullptr, 0, start, remote, 0, nullptr);
    if (!th) {
        *err = "De thread in het spel kon niet gestart worden (code " +
               std::to_string(GetLastError()) + ").";
        VirtualFreeEx(h, remote, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }

    WaitForSingleObject(th, 15000);
    DWORD module = 0;
    GetExitCodeThread(th, &module);
    CloseHandle(th);
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);

    if (module == 0) {
        *err = "Het spel gaf NULL terug bij het laden. Staat zp-freeze.dll "
               "naast dit programma, en is die 32-bit?";
        return false;
    }
    return true;
}

} // namespace zp
