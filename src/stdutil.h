#pragma once
#include "common.h"

#include <unordered_map>

namespace winged {

template<typename K, typename V>
const V * tryGet(const std::unordered_map<K, V> &collection, const K &key) {
    auto found = collection.find(key);
    return (found == collection.end()) ? nullptr : &found->second;
}

} // namespace
