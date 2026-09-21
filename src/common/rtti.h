// rtti.h - MSVC RTTI uitlezen uit een draaiend proces.
//
// Werkt alleen als de binary met /GR is gebouwd (RTTI aan). Zo niet, dan valt
// de probe terug op de heuristiek in heuristic.h, die geen RTTI nodig heeft.
//
// Indeling in een 32-bit image:
//
//   TypeDescriptor            COL (RTTICompleteObjectLocator)   vtable
//   +0x00 pVFTable            +0x00 signature (0)               -0x04 -> COL
//   +0x04 spare               +0x04 offset                      +0x00 slot 0
//   +0x08 naam ".?AVFoo@@"    +0x08 cdOffset                    +0x04 slot 1
//                             +0x0C pTypeDescriptor             ...
//                             +0x10 pClassDescriptor
//
// In een 64-bit image staat de naam op +0x10 en zijn de COL-velden
// image-relatieve DWORDs; signature is dan 1 en er is een extra pSelf op +0x14.
#pragma once

#include "target.h"
#include <map>

namespace zp {

struct VtableInfo {
    std::string cls;          // uitgepakte klassenaam, bv. "ActiveBody"
    std::string mangled;      // ".?AVActiveBody@@"
    uint64_t    typeDesc = 0;
    uint64_t    col = 0;
    uint32_t    subobjectOffset = 0;  // COL::offset: waar dit subobject begint
    uint64_t    vtable = 0;
    uint32_t    slotCount = 0;        // geschat aantal bruikbare slots
};

// Zoekt de vtables van de opgegeven klassen. Per klasse kunnen er meerdere
// zijn: bij meervoudige overerving heeft elk basis-subobject zijn eigen
// vtable, te onderscheiden via subobjectOffset.
std::vector<VtableInfo> scanRtti(const Target& t,
                                 const std::vector<std::string>& classNames);

// Leest de eerste n slots van een vtable. Slots die niet naar uitvoerbaar
// geheugen wijzen stoppen de lijst; dat markeert doorgaans het einde.
std::vector<uint64_t> readVtable(const Target& t, uint64_t vtable, uint32_t maxSlots);

// ".?AVActiveBody@@" -> "ActiveBody". Geeft "" terug bij een onbekende vorm.
std::string demangleTypeName(const std::string& mangled);

} // namespace zp
