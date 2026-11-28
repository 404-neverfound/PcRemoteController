#include "ClipboardImage.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <gdiplus.h>
#include <objbase.h>
#include <cstring>
#include <new>
#include <vector>

#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Ole32.lib")

namespace {
    const int kMaxDimension = 10000;
    const long long kMaxPixels = 30LL * 1000 * 1000;

    bool IsPng(const std::string& data) {
        static const unsigned char kSignature[8] = {
            0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
        };
        return data.size() >= sizeof(kSignature) &&
               memcmp(data.data(), kSignature, sizeof(kSignature)) == 0;
    }

    bool OpenClipboardWithRetry() {
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (OpenClipboard(NULL)) {
                return true;
            }
            Sleep(50);
        }
        return false;
    }

    // Places a copy of the given bytes on the clipboard in the given format.
    // The system owns the memory when the call succeeds.
    bool SetClipboardBlob(UINT format, const void* data, size_t size) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!memory) {
            return false;
        }
        void* target = GlobalLock(memory);
        if (!target) {
            GlobalFree(memory);
            return false;
        }
        memcpy(target, data, size);
        GlobalUnlock(memory);
        if (!SetClipboardData(format, memory)) {
            GlobalFree(memory);
            return false;
        }
        return true;
    }
}

namespace ClipboardImage {
    Result CopyToClipboard(const std::string& data) {
        Result result;

        Gdiplus::GdiplusStartupInput startup;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &startup, NULL) != Gdiplus::Ok) {
            result.error = "gdiplus unavailable";
            LOG("ClipboardImage: GdiplusStartup failed\n");
            return result;
        }

        IStream* stream = NULL;
        Gdiplus::Bitmap* bitmap = NULL;
        HBITMAP handle = NULL;
        std::vector<BYTE> pixels;
        BITMAPINFO info = {};

        if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) && stream) {
            ULONG written = 0;
            if (data.size() <= 0xFFFFFFFFull &&
                SUCCEEDED(stream->Write(data.data(), (ULONG)data.size(), &written)) &&
                written == data.size()) {
                LARGE_INTEGER zero = {};
                stream->Seek(zero, STREAM_SEEK_SET, NULL);
                bitmap = Gdiplus::Bitmap::FromStream(stream);
                if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
                    result.error = "invalid image";
                }
            } else {
                result.error = "out of memory";
            }
        } else {
            result.error = "out of memory";
        }

        if (bitmap && result.error.empty()) {
            UINT width = bitmap->GetWidth();
            UINT height = bitmap->GetHeight();
            if (width == 0 || height == 0 ||
                width > (UINT)kMaxDimension || height > (UINT)kMaxDimension ||
                (long long)width * height > kMaxPixels) {
                result.error = "image too large";
            } else if (bitmap->GetHBITMAP(Gdiplus::Color(255, 255, 255), &handle) !=
                           Gdiplus::Ok ||
                       !handle) {
                result.error = "invalid image";
            } else {
                int stride = ((int)width * 3 + 3) & ~3;
                size_t bytes = (size_t)stride * (size_t)height;
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = (LONG)width;
                // Positive height: GetDIBits returns bottom-up rows, which is
                // exactly what CF_DIB expects.
                info.bmiHeader.biHeight = (LONG)height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 24;
                info.bmiHeader.biCompression = BI_RGB;
                info.bmiHeader.biSizeImage = (DWORD)bytes;

                bool allocated = true;
                try {
                    pixels.resize(bytes);
                } catch (const std::bad_alloc&) {
                    allocated = false;
                }
                if (!allocated) {
                    result.error = "out of memory";
                } else {
                    HDC dc = CreateCompatibleDC(NULL);
                    if (!dc) {
                        result.error = "out of memory";
                    } else {
                        int lines = GetDIBits(dc, handle, 0, height,
                                              pixels.data(), &info, DIB_RGB_COLORS);
                        DeleteDC(dc);
                        if (lines != (int)height) {
                            result.error = "invalid image";
                        }
                    }
                }
                DeleteObject(handle);
                handle = NULL;

                if (result.error.empty()) {
                    if (!OpenClipboardWithRetry()) {
                        DWORD error = GetLastError();
                        LOG("ClipboardImage: OpenClipboard failed: %lu\n", error);
                        result.error = "clipboard busy";
                    } else {
                        bool copied = false;
                        DWORD clipboardError = 0;
                        if (EmptyClipboard()) {
                            size_t dibSize = sizeof(BITMAPINFOHEADER) + bytes;
                            HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, dibSize);
                            if (dib) {
                                void* target = GlobalLock(dib);
                                if (target) {
                                    memcpy(target, &info.bmiHeader,
                                           sizeof(BITMAPINFOHEADER));
                                    memcpy((BYTE*)target + sizeof(BITMAPINFOHEADER),
                                           pixels.data(), bytes);
                                    GlobalUnlock(dib);
                                    if (SetClipboardData(CF_DIB, dib)) {
                                        dib = NULL; // owned by the clipboard
                                        copied = true;
                                    }
                                }
                                if (dib) {
                                    GlobalFree(dib);
                                }
                            }
                        }
                        if (!copied) {
                            clipboardError = GetLastError();
                        }
                        if (copied && IsPng(data)) {
                            UINT pngFormat = RegisterClipboardFormatW(L"PNG");
                            if (pngFormat != 0 &&
                                !SetClipboardBlob(pngFormat, data.data(), data.size())) {
                                DWORD error = GetLastError();
                                LOG("ClipboardImage: PNG clipboard format failed: %lu\n",
                                    error);
                            }
                        }
                        CloseClipboard();

                        if (copied) {
                            result.ok = true;
                            result.width = (int)width;
                            result.height = (int)height;
                        } else {
                            LOG("ClipboardImage: clipboard write failed: %lu\n",
                                clipboardError);
                            result.error = "clipboard busy";
                        }
                    }
                }
            }
        }

        if (handle) {
            DeleteObject(handle);
        }
        delete bitmap;
        if (stream) {
            stream->Release();
        }
        Gdiplus::GdiplusShutdown(token);
        return result;
    }
}
