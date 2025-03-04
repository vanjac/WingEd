// Defines a unique identifier datatype id_t, to be used as a lookup key. This avoids the use of
// pointers to refer to objects, so all data can be DAGs, suitable for persistent data structures
// (like the ones in immer).
// id_t uses Windows GUIDs, so each ID will never be reused and broken references will always be
// detectable.

#pragma once
#include "common.h"

#include <memory>
#include <stdint.h>
#include <guiddef.h>

namespace winged {

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
            uint32_t dword = reinterpret_cast<const uint32_t *>(&key)[i];
            hash = hash * 23 + std::hash<uint32_t>()(dword);
        }
        return hash;
    }
};
