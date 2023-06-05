#pragma once
#include "common.h"

#include <stdint.h>
#include <guiddef.h>

namespace winged {

// unique identifier
using id_t = GUID;
id_t genId();

} // namespace


// operator== is already implemented for GUIDs in guiddef.h
template<>
struct std::hash<GUID> {
    std::size_t operator() (const GUID &key) const {
        // https://stackoverflow.com/a/263416
        size_t hash = 17;
        for (int i = 0; i < 4; i++) {
            uint32_t dword = ((uint32_t *)&key)[i];
            hash = hash * 23 + std::hash<uint32_t>()(dword);
        }
        return hash;
    }
};
