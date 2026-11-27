#include "DesktopNotify.h"
#include "../utils/Logger.h"
#include <windows.h>
#include <gdiplus.h>
#include <process.h>
#include <cerrno>
#include <new>
#include <string>

#pragma comment(lib, "Gdiplus.lib")

namespace {
    const wchar_t* kWindowClass = L"PcRemoteControllerNotifyWindow";
    const UINT_PTR kAnimationTimerId = 1;
    const UINT kAnimationIntervalMs = 20;
    const ULONGLONG kFadeInMs = 150;
    const ULONGLONG kHoldMs = 3000;
    const ULONGLONG kFadeOutMs = 300;

    struct Notification {
        std::wstring message;
    };

    struct RenderState {
        std::wstring message;
        HFONT messageFont;
        ULONGLONG startTick;
        int dpi;
        bool gdiplusReady;
    };

    int Scale(int value, int dpi) {
        return MulDiv(value, dpi, 96);
    }

    void DrawCheckIconGdi(HDC dc, const RECT& box) {
        int w = box.right - box.left;
        int h = box.bottom - box.top;

        HBRUSH circle = CreateSolidBrush(RGB(46, 160, 67));
        HPEN circlePen = CreatePen(PS_SOLID, 1, RGB(36, 128, 54));
        HGDIOBJ oldBrush = SelectObject(dc, circle);
        HGDIOBJ oldPen = SelectObject(dc, circlePen);
        Ellipse(dc, box.left, box.top, box.right, box.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(circle);
        DeleteObject(circlePen);

        int penWidth = w / 14;
        if (penWidth < 2) {
            penWidth = 2;
        }
        LOGBRUSH logBrush = {};
        logBrush.lbStyle = BS_SOLID;
        logBrush.lbColor = RGB(255, 255, 255);
        HPEN checkPen = ExtCreatePen(
            PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
            penWidth, &logBrush, 0, NULL);
        if (checkPen) {
            oldPen = SelectObject(dc, checkPen);
            POINT points[3] = {
                { box.left + w * 28 / 100, box.top + h * 52 / 100 },
                { box.left + w * 43 / 100, box.top + h * 67 / 100 },
                { box.left + w * 72 / 100, box.top + h * 35 / 100 },
            };
            Polyline(dc, points, 3);
            SelectObject(dc, oldPen);
            DeleteObject(checkPen);
        }
    }

    void DrawCheckIconGdiPlus(HDC dc, const RECT& box) {
        using namespace Gdiplus;

        Graphics graphics(dc);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);

        REAL left = static_cast<REAL>(box.left);
        REAL top = static_cast<REAL>(box.top);
        REAL width = static_cast<REAL>(box.right - box.left - 1);
        REAL height = static_cast<REAL>(box.bottom - box.top - 1);

        SolidBrush circle(Color(255, 46, 160, 67));
        Pen border(Color(255, 36, 128, 54), 1.0f);
        graphics.FillEllipse(&circle, left, top, width, height);
        graphics.DrawEllipse(&border, left, top, width, height);

        REAL penWidth = static_cast<REAL>((box.right - box.left) / 14);
        if (penWidth < 2.0f) {
            penWidth = 2.0f;
        }
        Pen check(Color(255, 255, 255, 255), penWidth);
        check.SetStartCap(LineCapRound);
        check.SetEndCap(LineCapRound);
        check.SetLineJoin(LineJoinRound);
        PointF points[3] = {
            PointF(left + width * 0.28f, top + height * 0.52f),
            PointF(left + width * 0.43f, top + height * 0.67f),
            PointF(left + width * 0.72f, top + height * 0.35f),
        };
        graphics.DrawLines(&check, points, 3);
    }

    void DrawCheckIcon(HDC dc, const RECT& box, bool gdiplusReady) {
        if (gdiplusReady) {
            DrawCheckIconGdiPlus(dc, box);
        } else {
            DrawCheckIconGdi(dc, box);
        }
    }

    LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        RenderState* state = reinterpret_cast<RenderState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            if (!state) {
                return 0;
            }
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client;
            GetClientRect(hwnd, &client);

            HBRUSH background = CreateSolidBrush(RGB(32, 34, 38));
            FillRect(dc, &client, background);
            DeleteObject(background);

            HBRUSH borderBrush = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(72, 76, 84));
            HGDIOBJ oldBrush = SelectObject(dc, borderBrush);
            HGDIOBJ oldPen = SelectObject(dc, borderPen);
            int radius = Scale(14, state->dpi);
            RoundRect(dc, 0, 0, client.right, client.bottom, radius, radius);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(borderPen);

            int iconSize = Scale(36, state->dpi);
            int iconTop = (client.bottom - iconSize) / 2;
            RECT iconBox = { Scale(18, state->dpi), iconTop,
                             Scale(18, state->dpi) + iconSize, iconTop + iconSize };
            DrawCheckIcon(dc, iconBox, state->gdiplusReady);

            int textLeft = iconBox.right + Scale(16, state->dpi);
            int textRight = client.right - Scale(18, state->dpi);
            RECT messageRect = { textLeft, 0, textRight, client.bottom };

            SetBkMode(dc, TRANSPARENT);
            if (state->messageFont) {
                HGDIOBJ oldFont = SelectObject(dc, state->messageFont);
                SetTextColor(dc, RGB(245, 247, 250));
                DrawTextW(dc, state->message.c_str(), -1, &messageRect,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                SelectObject(dc, oldFont);
            }

            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kAnimationTimerId && state) {
                ULONGLONG elapsed = GetTickCount64() - state->startTick;
                int alpha = 255;
                if (elapsed < kFadeInMs) {
                    alpha = static_cast<int>(255 * elapsed / kFadeInMs);
                } else if (elapsed >= kFadeInMs + kHoldMs) {
                    ULONGLONG fadeOut = elapsed - (kFadeInMs + kHoldMs);
                    if (fadeOut >= kFadeOutMs) {
                        KillTimer(hwnd, kAnimationTimerId);
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    alpha = 255 - static_cast<int>(255 * fadeOut / kFadeOutMs);
                }
                SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            if (state) {
                if (state->messageFont) {
                    DeleteObject(state->messageFont);
                }
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    unsigned __stdcall NotifyThread(void* param) {
        Notification* notification = static_cast<Notification*>(param);
        std::wstring message = notification->message;
        delete notification;

        // Use real pixels on scaled desktops (e.g. 150%) so the overlay has
        // crisp fonts and correct dimensions.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        UINT dpi = GetDpiForSystem();
        if (dpi == 0) {
            dpi = 96;
        }

        HINSTANCE instance = GetModuleHandleW(NULL);
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = NotifyWndProc;
        windowClass.hInstance = instance;
        // IDC_ARROW is MAKEINTRESOURCEA unless UNICODE is defined.
        windowClass.hCursor = LoadCursorW(NULL, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        windowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&windowClass)) {
            DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                LOG("DesktopNotify: RegisterClassEx failed: %lu\n", error);
                return 0;
            }
        }

        int width = Scale(400, dpi);
        int height = Scale(80, dpi);
        int margin = Scale(24, dpi);

        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        POINT origin = { 0, 0 };
        HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(monitor, &monitorInfo)) {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &monitorInfo.rcWork, 0);
        }
        int x = monitorInfo.rcWork.right - width - margin;
        int y = monitorInfo.rcWork.bottom - height - margin;

        RenderState* state = new (std::nothrow) RenderState();
        if (!state) {
            return 0;
        }
        state->message = message;
        state->messageFont = NULL;
        state->startTick = 0;
        state->dpi = static_cast<int>(dpi);
        state->gdiplusReady = false;

        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kWindowClass, L"", WS_POPUP, x, y, width, height,
            NULL, NULL, instance, state);
        if (!hwnd) {
            DWORD error = GetLastError();
            LOG("DesktopNotify: CreateWindowEx failed: %lu\n", error);
            delete state;
            return 0;
        }

        Gdiplus::GdiplusStartupInput gdiplusInput;
        ULONG_PTR gdiplusToken = 0;
        bool gdiplusReady =
            Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL) == Gdiplus::Ok;
        state->gdiplusReady = gdiplusReady;
        if (!gdiplusReady) {
            LOG("DesktopNotify: GdiplusStartup failed\n");
        }

        state->messageFont = CreateFontW(
            -Scale(13, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                         Scale(14, dpi), Scale(14, dpi));
        if (region && !SetWindowRgn(hwnd, region, FALSE)) {
            DeleteObject(region);
        }

        state->startTick = GetTickCount64();
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(hwnd, kAnimationTimerId, kAnimationIntervalMs, NULL);
        InvalidateRect(hwnd, NULL, TRUE);

        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (gdiplusReady) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
        return 0;
    }
}

namespace DesktopNotify {
    bool Show(const std::wstring& message) {
        Notification* notification = new (std::nothrow) Notification();
        if (!notification) {
            return false;
        }
        notification->message = message;

        HANDLE thread = reinterpret_cast<HANDLE>(
            _beginthreadex(NULL, 0, NotifyThread, notification, 0, NULL));
        if (!thread) {
            delete notification;
            LOG("DesktopNotify: _beginthreadex failed: errno=%d\n", errno);
            return false;
        }

        CloseHandle(thread);
        return true;
    }
}
