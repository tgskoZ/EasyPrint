#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/utils/Keyboard.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace geode::prelude;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

namespace {
    // Set when Print Screen is pressed; consumed by the next swapBuffers call,
    // right before the finished frame is presented.
    std::atomic_bool s_captureRequested = false;
    // True while a previous screenshot is still being copied / saved.
    std::atomic_bool s_busy = false;
    // True once the low-level keyboard hook is up. If it fails to install,
    // the in-game key listener triggers screenshots instead.
    std::atomic_bool s_hookActive = false;
    std::atomic_bool s_blockSystem = true;
    std::atomic_bool s_printDown = false;

    void requestCapture() {
        if (s_busy) return;
        s_captureRequested = true;
    }

    bool isGameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    // Windows itself reacts to Print Screen (Snipping Tool, screen capture
    // overlay, ...), which steals focus from a fullscreen game and can make it
    // freeze, minimize or crash. A low-level hook sees the key before anyone
    // else, so we can swallow it while the game is in front.
    LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HC_ACTION) {
            auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            if (info->vkCode == VK_SNAPSHOT && s_blockSystem && isGameFocused()) {
                bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
                if (down && !s_printDown.exchange(true)) {
                    requestCapture();
                }
                if (!down) {
                    s_printDown = false;
                }
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    void runHookThread() {
        std::thread([] {
            HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, GetModuleHandleW(nullptr), 0);
            if (!hook) {
                log::warn("Failed to install keyboard hook (error {}), using in-game key handling", GetLastError());
                return;
            }
            s_hookActive = true;
            log::info("Keyboard hook installed");

            // Low-level hooks are delivered through this thread's message loop.
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            UnhookWindowsHookEx(hook);
            s_hookActive = false;
        }).detach();
    }

    bool copyToClipboard(std::vector<uint8_t> const& dib) {
        if (!OpenClipboard(nullptr)) return false;
        EmptyClipboard();

        bool ok = false;
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, dib.size())) {
            if (void* dst = GlobalLock(mem)) {
                std::memcpy(dst, dib.data(), dib.size());
                GlobalUnlock(mem);
                // On success the clipboard owns the memory.
                ok = SetClipboardData(CF_DIB, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }

        CloseClipboard();
        return ok;
    }

    Result<std::filesystem::path> saveToFile(std::vector<uint8_t> const& dib) {
        auto dir = Mod::get()->getSaveDir() / "screenshots";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return Err("Unable to create folder: {}", ec.message());

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        std::tm tm{};
        localtime_s(&tm, &time);
        char name[64];
        std::snprintf(
            name, sizeof(name), "screenshot_%04d-%02d-%02d_%02d-%02d-%02d-%03d.bmp",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms)
        );
        auto path = dir / name;

        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42; // "BM"
        header.bfSize = static_cast<DWORD>(sizeof(header) + dib.size());
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

        std::ofstream file(path, std::ios::binary);
        if (!file) return Err("Unable to open file");
        file.write(reinterpret_cast<char const*>(&header), sizeof(header));
        file.write(reinterpret_cast<char const*>(dib.data()), dib.size());
        if (!file) return Err("Unable to write file");
        return Ok(path);
    }

    void notify(std::string text, NotificationIcon icon) {
        Loader::get()->queueInMainThread([text = std::move(text), icon] {
            if (icon == NotificationIcon::Success && !Mod::get()->getSettingValue<bool>("show-notification")) {
                return;
            }
            Notification::create(text, icon)->show();
        });
    }

    // Reads the frame that is about to be presented and hands it off to a
    // worker thread, so the game doesn't stutter while copying / saving.
    void captureBackBuffer() {
        HWND window = WindowFromDC(wglGetCurrentDC());
        RECT rect{};
        if (!window || !GetClientRect(window, &rect)) {
            notify("EasyPrint: couldn't find the game window", NotificationIcon::Error);
            return;
        }
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return;

        // 24-bit DIB rows are padded to 4 bytes, which is exactly what
        // glReadPixels produces with GL_PACK_ALIGNMENT = 4. OpenGL also
        // returns rows bottom-up, same as a DIB, so no conversion is needed.
        size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~size_t(3);
        size_t imageSize = stride * height;

        std::vector<uint8_t> dib(sizeof(BITMAPINFOHEADER) + imageSize);
        auto* info = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
        info->biSize = sizeof(BITMAPINFOHEADER);
        info->biWidth = width;
        info->biHeight = height;
        info->biPlanes = 1;
        info->biBitCount = 24;
        info->biCompression = BI_RGB;
        info->biSizeImage = static_cast<DWORD>(imageSize);

        GLint prevFramebuffer = 0;
        GLint prevAlignment = 4;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlignment);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, dib.data() + sizeof(BITMAPINFOHEADER));
        GLenum error = glGetError();

        glPixelStorei(GL_PACK_ALIGNMENT, prevAlignment);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFramebuffer);

        if (error != GL_NO_ERROR) {
            log::error("glReadPixels failed: {}", error);
            notify("EasyPrint: failed to capture the screen", NotificationIcon::Error);
            return;
        }

        bool saveFile = Mod::get()->getSettingValue<bool>("save-to-file");
        s_busy = true;
        std::thread([dib = std::move(dib), saveFile, width, height] {
            bool copied = copyToClipboard(dib);
            if (!copied) {
                log::error("Failed to copy screenshot to clipboard (error {})", GetLastError());
            }

            bool saved = false;
            if (saveFile) {
                if (auto res = saveToFile(dib); res.isOk()) {
                    saved = true;
                    log::info("Saved screenshot to {}", utils::string::pathToString(res.unwrap()));
                } else {
                    log::error("Failed to save screenshot: {}", res.unwrapErr());
                }
            }

            if (copied) {
                notify(
                    fmt::format("Screenshot copied! ({}x{}){}", width, height, saved ? "\nSaved to file" : ""),
                    NotificationIcon::Success
                );
            } else if (saved) {
                notify("Couldn't copy to clipboard, saved to file", NotificationIcon::Warning);
            } else {
                notify("EasyPrint: failed to copy screenshot", NotificationIcon::Error);
            }
            s_busy = false;
        }).detach();
    }
}

class $modify(EasyPrintGLView, CCEGLView) {
    void swapBuffers() {
        if (s_captureRequested.exchange(false)) {
            captureBackBuffer();
        }
        CCEGLView::swapBuffers();
    }
};

$on_mod(Loaded) {
    s_blockSystem = Mod::get()->getSettingValue<bool>("block-system-screenshot");
    listenForSettingChanges<bool>("block-system-screenshot", [](bool value) {
        s_blockSystem = value;
    });

    runHookThread();

    // Never let Print Screen reach the game itself. When the low-level hook
    // is not active (failed to install or disabled in settings) this is what
    // triggers the screenshot.
    KeyboardInputEvent(KEY_PrintScreen).listen([](KeyboardInputData& data) {
        if (data.action == KeyboardInputData::Action::Press && !(s_hookActive && s_blockSystem)) {
            requestCapture();
        }
        return ListenerResult::Stop;
    }).leak();
}
