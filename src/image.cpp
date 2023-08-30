#include "image.h"

#include "winchroma.h"
#include <objidl.h>
// GDI+ requires min/max
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif
#pragma warning(disable: 4458)
#include <gdiplus.h>

namespace winged {

namespace Gdi = Gdiplus;

static ULONG_PTR gdiplusToken = 0;

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
    checkStatus(Gdi::GdiplusStartup(&gdiplusToken, &input, &output));
}

void uninitImage() {
    Gdi::GdiplusShutdown(gdiplusToken);
}

ImageData loadImage(const wchar_t *path) {
    ImageData image;
    Gdi::Bitmap bitmap(path, FALSE);
    if (!checkStatus(bitmap.GetLastStatus()))
        return image;
    Gdi::Rect rect(0, 0, bitmap.GetWidth(), bitmap.GetHeight());
    int stride = rect.Width * 4;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[stride * rect.Height]);
    Gdi::BitmapData data = {
        (UINT)rect.Width, (UINT)rect.Height, stride, PixelFormat32bppARGB, buffer.get(), 0
    };
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