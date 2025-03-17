// Convenient wrapper for inconvenient GDI+

#pragma once
#include "common.h"

#include <memory>
#include <string>

namespace winged {

struct ImageData {
    std::unique_ptr<uint8_t[]> data; // BGRA 32-bit
    int width = 0, height = 0;
};

void initImage();
void uninitImage();

ImageData loadImage(const std::string &path);

} // namespace
