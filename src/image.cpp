#include "image.h"

#include "winchroma.h"
#include "strutil.h"
#include <objidl.h>
#ifndef __MINGW32__
// GDI+ requires min/max
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#endif // __MINGW32__
#pragma warning(disable: 4458)
#include <gdiplus.h>

namespace Gdi = Gdiplus;

namespace winged {

static ULONG_PTR g_gdiplusToken = 0;

bool checkStatus(Gdi::Status status) {
    if (status != Gdi::Status::Ok) {
#ifdef CHROMA_DEBUG
        wprintf(L"GDI+ error %d\n", status);
#endif
        return false;
    }
    return true;
}

void initImage() {
    Gdi::GdiplusStartupInput input;
    Gdi::GdiplusStartupOutput output;
    checkStatus(Gdi::GdiplusStartup(&g_gdiplusToken, &input, &output));
}

void uninitImage() {
    Gdi::GdiplusShutdown(g_gdiplusToken);
}

ImageData loadImage(const std::string &path) {
    ImageData image;
    Gdi::Bitmap bitmap(widen(path).c_str(), FALSE);
    if (!checkStatus(bitmap.GetLastStatus()))
        return image;
    Gdi::Rect rect(0, 0, bitmap.GetWidth(), bitmap.GetHeight());
    auto stride = -rect.Width * 4; // bottom-up order
    auto bufSize = rect.Width * rect.Height * 4;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufSize]);
    Gdi::BitmapData data = { UINT(rect.Width), UINT(rect.Height), stride, PixelFormat32bppARGB,
        buffer.get() + bufSize + stride, 0 };
    if (!checkStatus(bitmap.LockBits(&rect, Gdi::ImageLockModeRead | Gdi::ImageLockModeUserInputBuf,
            PixelFormat32bppARGB, &data))) {
        return image;
    }
    if (!checkStatus(bitmap.UnlockBits(&data)))
        return image;
    image.data = std::move(buffer);
    image.width = rect.Width;
    image.height = rect.Height;
    return image;
}

} // namespace
