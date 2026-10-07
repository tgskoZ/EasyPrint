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
    // Set when a screenshot should be taken; consumed by the next swapBuffers
    // call, right before the finished frame is presented.
    std::atomic_bool s_captureRequested = false;
    // True while a previous screenshot is still being saved.
    std::atomic_bool s_busy = false;
    // True once the low-level keyboard hook is up. If it fails to install,
    // the in-game key listener triggers screenshots instead.
    std::atomic_bool s_hookActive = false;
    std::atomic_bool s_blockSystem = true;
    std::atomic_bool s_printDown = false;

    // A 24-bit bottom-up DIB: BITMAPINFOHEADER followed by the pixels.
    struct Screenshot {
        std::vector<uint8_t> dib;
        int width = 0;
        int height = 0;
    };

    void captureWithRenderTexture();

    HWND getGameWindow() {
        if (HWND window = WindowFromDC(wglGetCurrentDC())) {
            return window;
        }
        DWORD pid = 0;
        HWND foreground = GetForegroundWindow();
        GetWindowThreadProcessId(foreground, &pid);
        return pid == GetCurrentProcessId() ? foreground : nullptr;
    }

    bool isGameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    // Must be called on the main thread.
    void startCapture() {
        if (s_busy || s_captureRequested) return;
        log::info("Print Screen pressed, taking screenshot");
        s_captureRequested = true;

        // Normally the swapBuffers hook grabs the frame this very frame. If it
        // didn't (hook not called for some reason), render the scene ourselves
        // on the next frame instead.
        Loader::get()->queueInMainThread([] {
            Loader::get()->queueInMainThread([] {
                if (s_captureRequested.exchange(false)) {
                    log::warn("swapBuffers capture didn't happen, using render texture fallback");
                    captureWithRenderTexture();
                }
            });
        });
    }

    // Safe to call from any thread.
    void requestCapture() {
        Loader::get()->queueInMainThread(&startCapture);
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

    Screenshot makeScreenshot(int width, int height) {
        // 24-bit DIB rows are padded to 4 bytes.
        size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~size_t(3);
        size_t imageSize = stride * height;

        Screenshot shot;
        shot.width = width;
        shot.height = height;
        shot.dib.resize(sizeof(BITMAPINFOHEADER) + imageSize);

        auto* info = reinterpret_cast<BITMAPINFOHEADER*>(shot.dib.data());
        info->biSize = sizeof(BITMAPINFOHEADER);
        info->biWidth = width;
        info->biHeight = height;
        info->biPlanes = 1;
        info->biBitCount = 24;
        info->biCompression = BI_RGB;
        info->biSizeImage = static_cast<DWORD>(imageSize);
        return shot;
    }

    uint8_t* pixels(Screenshot& shot) {
        return shot.dib.data() + sizeof(BITMAPINFOHEADER);
    }

    bool copyToClipboard(Screenshot const& shot) {
        // The clipboard needs an owner window: with a null owner,
        // EmptyClipboard makes the following SetClipboardData fail.
        HWND owner = getGameWindow();
        bool opened = false;
        for (int i = 0; i < 5 && !(opened = OpenClipboard(owner)); i++) {
            // Another app may be holding the clipboard for a moment.
            Sleep(10);
        }
        if (!opened) {
            log::error("OpenClipboard failed (error {})", GetLastError());
            return false;
        }
        EmptyClipboard();

        bool ok = false;
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, shot.dib.size())) {
            if (void* dst = GlobalLock(mem)) {
                std::memcpy(dst, shot.dib.data(), shot.dib.size());
                GlobalUnlock(mem);
                // On success the clipboard owns the memory.
                ok = SetClipboardData(CF_DIB, mem) != nullptr;
                if (!ok) log::error("SetClipboardData failed (error {})", GetLastError());
            }
            if (!ok) GlobalFree(mem);
        }

        CloseClipboard();
        return ok;
    }

    Result<std::filesystem::path> saveToFile(Screenshot const& shot) {
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
        header.bfSize = static_cast<DWORD>(sizeof(header) + shot.dib.size());
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

        std::ofstream file(path, std::ios::binary);
        if (!file) return Err("Unable to open file");
        file.write(reinterpret_cast<char const*>(&header), sizeof(header));
        file.write(reinterpret_cast<char const*>(shot.dib.data()), shot.dib.size());
        if (!file) return Err("Unable to write file");
        return Ok(path);
    }

    void notify(std::string const& text, NotificationIcon icon) {
        if (icon == NotificationIcon::Success && !Mod::get()->getSettingValue<bool>("show-notification")) {
            return;
        }
        Notification::create(text, icon)->show();
    }

    // Main thread: copy to the clipboard right away (it's just a memcpy),
    // then save the file on a worker thread if enabled.
    void deliver(Screenshot shot) {
        bool copied = copyToClipboard(shot);
        log::info("Screenshot {}x{} {}", shot.width, shot.height, copied ? "copied to clipboard" : "NOT copied");

        if (!Mod::get()->getSettingValue<bool>("save-to-file")) {
            if (copied) {
                notify(fmt::format("Screenshot copied! ({}x{})", shot.width, shot.height), NotificationIcon::Success);
            } else {
                notify("EasyPrint: failed to copy screenshot", NotificationIcon::Error);
            }
            return;
        }

        s_busy = true;
        std::thread([shot = std::move(shot), copied] {
            auto res = saveToFile(shot);
            if (res.isOk()) {
                log::info("Saved screenshot to {}", utils::string::pathToString(res.unwrap()));
            } else {
                log::error("Failed to save screenshot: {}", res.unwrapErr());
            }
            bool saved = res.isOk();
            int width = shot.width, height = shot.height;

            Loader::get()->queueInMainThread([copied, saved, width, height] {
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
            });
        }).detach();
    }

    // Reads the frame that is about to be presented, at the window's size.
    void captureBackBuffer() {
        HWND window = getGameWindow();
        RECT rect{};
        if (!window || !GetClientRect(window, &rect)) {
            log::warn("Couldn't get game window, using render texture fallback");
            captureWithRenderTexture();
            return;
        }
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return;

        auto shot = makeScreenshot(width, height);

        GLint prevFramebuffer = 0;
        GLint prevAlignment = 4;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlignment);
        while (glGetError() != GL_NO_ERROR) {}

        // With GL_PACK_ALIGNMENT = 4 the rows come out padded exactly like a
        // DIB, and OpenGL returns them bottom-up just like a DIB too.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, pixels(shot));
        GLenum error = glGetError();

        glPixelStorei(GL_PACK_ALIGNMENT, prevAlignment);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFramebuffer);

        if (error != GL_NO_ERROR) {
            log::warn("glReadPixels failed ({}), using render texture fallback", error);
            captureWithRenderTexture();
            return;
        }
        deliver(std::move(shot));
    }

    // Fallback: render the current scene into an offscreen texture.
    void captureWithRenderTexture() {
        auto director = CCDirector::get();
        auto scene = director->getRunningScene();
        if (!scene) {
            notify("EasyPrint: failed to capture the screen", NotificationIcon::Error);
            return;
        }

        auto size = director->getWinSize();
        auto rt = CCRenderTexture::create(
            static_cast<int>(size.width), static_cast<int>(size.height), kCCTexture2DPixelFormat_RGBA8888
        );
        if (!rt) {
            notify("EasyPrint: failed to capture the screen", NotificationIcon::Error);
            return;
        }
        rt->begin();
        scene->visit();
        rt->end();

        // Not flipped: rows stay bottom-up, as a DIB wants them.
        auto image = rt->newCCImage(false);
        if (!image) {
            notify("EasyPrint: failed to capture the screen", NotificationIcon::Error);
            return;
        }

        int width = image->getWidth();
        int height = image->getHeight();
        auto shot = makeScreenshot(width, height);
        size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~size_t(3);
        uint8_t const* src = image->getData();
        uint8_t* dst = pixels(shot);
        for (int y = 0; y < height; y++) {
            uint8_t const* in = src + static_cast<size_t>(y) * width * 4;
            uint8_t* out = dst + static_cast<size_t>(y) * stride;
            for (int x = 0; x < width; x++) {
                out[x * 3 + 0] = in[x * 4 + 2];
                out[x * 3 + 1] = in[x * 4 + 1];
                out[x * 3 + 2] = in[x * 4 + 0];
            }
        }
        image->release();

        deliver(std::move(shot));
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
