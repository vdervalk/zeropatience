// inject.h - de DLL in het spel laden.
//
// Geheugen reserveren in het doelproces, daar het pad van de DLL neerzetten,
// en een thread starten op LoadLibraryA. Kernel32 ligt in elk 32-bit proces op
// hetzelfde adres, dus het adres uit ons eigen proces geldt daar ook.
#pragma once

#include <windows.h>
#include <string>

namespace zp {

// Geeft true bij succes. Bij falen staat de reden in err, in gewone taal.
bool injectDll(DWORD pid, const std::string& dllPath, std::string* err);

// Het pad van een bestand naast de draaiende executable.
std::string pathNextToExe(const char* name);

} // namespace zp
