#pragma once
#include <string>

namespace ClipboardImage {
    struct Result {
        bool ok = false;
        int width = 0;
        int height = 0;
        // ASCII token. Empty on success; otherwise one of "invalid image",
        // "image too large", "clipboard busy", "out of memory" or
        // "gdiplus unavailable".
        std::string error;
    };

    // Decodes an image from memory (PNG/JPEG/BMP/GIF/TIFF through the system
    // GDI+ codecs) and writes it to the clipboard as CF_DIB. When the input is
    // a PNG the original encoded bytes are also published under the registered
    // "PNG" clipboard format so transparent images and browser-based targets
    // keep their alpha channel. The previous clipboard contents are lost.
    Result CopyToClipboard(const std::string& data);
}
