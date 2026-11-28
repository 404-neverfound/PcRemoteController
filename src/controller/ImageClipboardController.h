#pragma once
#include "BaseController.h"

// Imports an image into the interactive session's clipboard. Supported forms:
//   复制图片 -base64          Base64 text from stdin (iOS Shortcuts / a-Shell)
//   复制图片 <path>           a local image file
//   最新截图 [dir]            the newest image in a directory, or by default
//                            latest.* in %USERPROFILE% and
//                            Pictures\PhoneScreenshots
// The CLI reads local files itself and streams the bytes to the agent, so the
// non-elevated agent never needs to open a high-integrity uploaded file.
class ImageClipboardController : public BaseController {
public:
    bool Match(const std::string& cmd) override;
    bool Execute(const std::string& cmd) override;
};
