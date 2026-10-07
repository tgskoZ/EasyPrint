#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/utils/Keyboard.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace {
    // Set when a screenshot should be taken; consumed by the next swapBuffers
    // call, right before the finished frame is presented.
    std::atomic_bool s_captureRequested = false;
    // True once the low-level keyboard hook is up. If it fails to install,
    // the in-game key listener triggers screenshots instead.
    std::atomic_bool s_hookActive = false;
    std::atomic_bool s_blockSystem = true;

    // A captured frame: RGBA, rows top-down.
    struct Frame {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
    };

    // A 24-bit bottom-up DIB: BITMAPINFOHEADER followed by the pixels.
    struct Screenshot {
        std::vector<uint8_t> dib;
        int width = 0;
        int height = 0;
    };

    void onFrameCaptured(Frame frame);
    bool isSelecting();

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

    void notify(std::string const& text, NotificationIcon icon) {
        if (icon == NotificationIcon::Success && !Mod::get()->getSettingValue<bool>("show-notification")) {
            return;
        }
        Notification::create(text, icon)->show();
    }

    // ---------------------------------------------------------------------
    // Capturing the frame
    // ---------------------------------------------------------------------

    // Fallback: render the current scene into an offscreen texture.
    void captureWithRenderTexture() {
        auto director = CCDirector::get();
        auto scene = director->getRunningScene();
        auto size = director->getWinSize();
        auto rt = scene ? CCRenderTexture::create(
            static_cast<int>(size.width), static_cast<int>(size.height), kCCTexture2DPixelFormat_RGBA8888
        ) : nullptr;
        if (!rt) {
            notify("EasyPrint: failed to capture the screen", NotificationIcon::Error);
            return;
        }
        rt->begin();
        scene->visit();
        rt->end();

        auto image = rt->newCCImage(true);
        if (!image) {
            notify("EasyPrint: failed to capture the screen", NotificationIcon::Error);
            return;
        }
        Frame frame;
        frame.width = image->getWidth();
        frame.height = image->getHeight();
        frame.rgba.assign(image->getData(), image->getData() + static_cast<size_t>(frame.width) * frame.height * 4);
        image->release();
        onFrameCaptured(std::move(frame));
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

        std::vector<uint8_t> raw(static_cast<size_t>(width) * height * 4);

        GLint prevFramebuffer = 0;
        GLint prevAlignment = 4;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFramebuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlignment);
        while (glGetError() != GL_NO_ERROR) {}

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        GLenum error = glGetError();

        glPixelStorei(GL_PACK_ALIGNMENT, prevAlignment);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFramebuffer);

        if (error != GL_NO_ERROR) {
            log::warn("glReadPixels failed ({}), using render texture fallback", error);
            captureWithRenderTexture();
            return;
        }

        // OpenGL gives rows bottom-up; flip them and make the frame opaque.
        Frame frame;
        frame.width = width;
        frame.height = height;
        frame.rgba.resize(raw.size());
        size_t rowSize = static_cast<size_t>(width) * 4;
        for (int y = 0; y < height; y++) {
            uint8_t* dst = frame.rgba.data() + (height - 1 - y) * rowSize;
            std::memcpy(dst, raw.data() + y * rowSize, rowSize);
            for (size_t x = 3; x < rowSize; x += 4) dst[x] = 255;
        }

        // Open the selection on the next frame, outside of rendering.
        Loader::get()->queueInMainThread([frame = std::move(frame)]() mutable {
            onFrameCaptured(std::move(frame));
        });
    }

    // Must be called on the main thread.
    void startCapture() {
        if (isSelecting() || s_captureRequested) return;
        log::info("Print Screen pressed, capturing frame");
        s_captureRequested = true;

        // Normally the swapBuffers hook grabs the frame this very frame. If it
        // didn't, render the scene ourselves on the next frame instead.
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

    // ---------------------------------------------------------------------
    // Clipboard / file output
    // ---------------------------------------------------------------------

    // Crops a region (in pixels, top-left origin) of the frame into a DIB.
    Screenshot crop(Frame const& frame, int x0, int y0, int width, int height) {
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

        uint8_t* pixels = shot.dib.data() + sizeof(BITMAPINFOHEADER);
        for (int row = 0; row < height; row++) {
            // DIB rows are bottom-up.
            int srcY = y0 + (height - 1 - row);
            uint8_t const* in = frame.rgba.data() + (static_cast<size_t>(srcY) * frame.width + x0) * 4;
            uint8_t* out = pixels + row * stride;
            for (int x = 0; x < width; x++) {
                out[x * 3 + 0] = in[x * 4 + 2];
                out[x * 3 + 1] = in[x * 4 + 1];
                out[x * 3 + 2] = in[x * 4 + 0];
            }
        }
        return shot;
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

    // Main thread: copy to the clipboard right away, then save the file on a
    // worker thread if enabled.
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
            });
        }).detach();
    }

    // ---------------------------------------------------------------------
    // Area selection
    // ---------------------------------------------------------------------

    // Shows the frozen frame and lets the player drag out the area to copy,
    // like the Windows snipping tool but inside the game.
    class SelectionOverlay : public CCNode {
    public:
        static inline SelectionOverlay* s_current = nullptr;

        static SelectionOverlay* create(Frame frame) {
            auto ret = new SelectionOverlay();
            if (ret->init(std::move(frame))) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }

        void onMouseButton(MouseInputData const& data) {
            using enum MouseInputData::Action;
            using enum MouseInputData::Button;
            if (data.button == Right && data.action == Press) {
                this->close();
            } else if (data.button == Left && data.action == Press) {
                m_start = m_end = this->mousePos();
                m_dragging = true;
                this->redraw();
            } else if (data.button == Left && data.action == Release && m_dragging) {
                m_end = this->mousePos();
                m_dragging = false;
                this->finish();
            }
        }

        void onKey(KeyboardInputData const& data) {
            if (data.action != KeyboardInputData::Action::Press) return;
            if (data.key == KEY_Escape) {
                this->close();
            } else if (data.key == KEY_Enter || data.key == KEY_Space) {
                this->copy(0, 0, m_frame.width, m_frame.height);
            }
        }

    protected:
        Frame m_frame;
        CCDrawNode* m_draw = nullptr;
        CCLabelBMFont* m_sizeLabel = nullptr;
        CCPoint m_start;
        CCPoint m_end;
        bool m_dragging = false;

        bool init(Frame frame) {
            if (!CCNode::init()) return false;
            m_frame = std::move(frame);

            auto winSize = CCDirector::get()->getWinSize();
            this->setContentSize(winSize);

            auto texture = new CCTexture2D();
            if (!texture->initWithData(
                m_frame.rgba.data(), kCCTexture2DPixelFormat_RGBA8888,
                m_frame.width, m_frame.height, CCSize(m_frame.width, m_frame.height)
            )) {
                texture->release();
                return false;
            }
            auto sprite = CCSprite::createWithTexture(texture);
            texture->release();
            sprite->setAnchorPoint({0, 0});
            sprite->setScaleX(winSize.width / sprite->getContentSize().width);
            sprite->setScaleY(winSize.height / sprite->getContentSize().height);
            this->addChild(sprite);

            m_draw = CCDrawNode::create();
            this->addChild(m_draw);

            auto hint = CCLabelBMFont::create("Drag to select an area   Enter: full screen   Esc: cancel", "bigFont.fnt");
            hint->setScale(std::min(0.4f, (winSize.width - 20.f) / hint->getContentSize().width));
            hint->setPosition(winSize.width / 2, winSize.height - 12.f);
            hint->setOpacity(220);
            this->addChild(hint);

            m_sizeLabel = CCLabelBMFont::create("", "bigFont.fnt");
            m_sizeLabel->setScale(0.3f);
            m_sizeLabel->setAnchorPoint({0, 1});
            m_sizeLabel->setVisible(false);
            this->addChild(m_sizeLabel);

            this->redraw();
            this->scheduleUpdate();
            return true;
        }

        void onEnter() override {
            CCNode::onEnter();
            s_current = this;
        }

        void onExit() override {
            if (s_current == this) s_current = nullptr;
            CCNode::onExit();
        }

        void update(float) override {
            if (m_dragging) {
                m_end = this->mousePos();
                this->redraw();
            }
        }

        CCPoint mousePos() const {
            auto pos = geode::cocos::getMousePos();
            auto size = this->getContentSize();
            return {std::clamp(pos.x, 0.f, size.width), std::clamp(pos.y, 0.f, size.height)};
        }

        CCRect selection() const {
            float x0 = std::min(m_start.x, m_end.x);
            float y0 = std::min(m_start.y, m_end.y);
            return {x0, y0, std::abs(m_end.x - m_start.x), std::abs(m_end.y - m_start.y)};
        }

        // Selection in frame pixels, top-left origin.
        void selectionPixels(int& x, int& y, int& w, int& h) const {
            auto size = this->getContentSize();
            auto rect = this->selection();
            float sx = m_frame.width / size.width;
            float sy = m_frame.height / size.height;
            int left = static_cast<int>(std::floor(rect.getMinX() * sx));
            int right = static_cast<int>(std::ceil(rect.getMaxX() * sx));
            int top = static_cast<int>(std::floor((size.height - rect.getMaxY()) * sy));
            int bottom = static_cast<int>(std::ceil((size.height - rect.getMinY()) * sy));
            x = std::clamp(left, 0, m_frame.width);
            y = std::clamp(top, 0, m_frame.height);
            w = std::clamp(right, 0, m_frame.width) - x;
            h = std::clamp(bottom, 0, m_frame.height) - y;
        }

        void redraw() {
            m_draw->clear();
            auto size = this->getContentSize();
            ccColor4F dim = {0.f, 0.f, 0.f, 0.45f};
            ccColor4F white = {1.f, 1.f, 1.f, 1.f};
            ccColor4F none = {0.f, 0.f, 0.f, 0.f};

            auto fill = [&](float x0, float y0, float x1, float y1) {
                if (x1 <= x0 || y1 <= y0) return;
                CCPoint verts[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
                m_draw->drawPolygon(verts, 4, dim, 0.f, none);
            };

            auto hasSelection = m_dragging || !m_start.equals(m_end);
            if (!hasSelection) {
                fill(0, 0, size.width, size.height);
                m_sizeLabel->setVisible(false);
                return;
            }

            auto r = this->selection();
            fill(0, 0, size.width, r.getMinY());
            fill(0, r.getMaxY(), size.width, size.height);
            fill(0, r.getMinY(), r.getMinX(), r.getMaxY());
            fill(r.getMaxX(), r.getMinY(), size.width, r.getMaxY());

            CCPoint a{r.getMinX(), r.getMinY()}, b{r.getMaxX(), r.getMinY()};
            CCPoint c{r.getMaxX(), r.getMaxY()}, d{r.getMinX(), r.getMaxY()};
            m_draw->drawSegment(a, b, 0.5f, white);
            m_draw->drawSegment(b, c, 0.5f, white);
            m_draw->drawSegment(c, d, 0.5f, white);
            m_draw->drawSegment(d, a, 0.5f, white);

            int x, y, w, h;
            this->selectionPixels(x, y, w, h);
            m_sizeLabel->setString(fmt::format("{}x{}", w, h).c_str());
            m_sizeLabel->setPosition(r.getMinX() + 2.f, r.getMinY() - 2.f);
            m_sizeLabel->setVisible(true);
        }

        void finish() {
            int x, y, w, h;
            this->selectionPixels(x, y, w, h);
            if (w < 4 || h < 4) {
                // Just a click: start over.
                m_start = m_end = CCPointZero;
                this->redraw();
                return;
            }
            this->copy(x, y, w, h);
        }

        void copy(int x, int y, int w, int h) {
            auto shot = crop(m_frame, x, y, w, h);
            this->close();
            deliver(std::move(shot));
        }

        void close() {
            this->retain();
            this->removeFromParent();
            if (s_current == this) s_current = nullptr;
            this->release();
        }
    };

    bool isSelecting() {
        return SelectionOverlay::s_current != nullptr;
    }

    void onFrameCaptured(Frame frame) {
        if (!Mod::get()->getSettingValue<bool>("select-area")) {
            deliver(crop(frame, 0, 0, frame.width, frame.height));
            return;
        }
        if (isSelecting()) return;

        // The frame is already frozen in the overlay; pause the level so the
        // player doesn't die while selecting.
        if (auto pl = PlayLayer::get(); pl && !pl->m_isPaused && !pl->m_hasCompletedLevel) {
            pl->pauseGame(false);
        }

        auto overlay = SelectionOverlay::create(std::move(frame));
        if (!overlay) {
            notify("EasyPrint: failed to open area selection", NotificationIcon::Error);
            return;
        }
        if (auto manager = OverlayManager::get()) {
            manager->addChild(overlay, 1000);
        } else if (auto scene = CCDirector::get()->getRunningScene()) {
            scene->addChild(overlay, 100000);
        }
    }

    // ---------------------------------------------------------------------
    // Print Screen key
    // ---------------------------------------------------------------------

    // Windows itself reacts to Print Screen (Snipping Tool, screen capture
    // overlay, ...), which steals focus from a fullscreen game and can make it
    // freeze, minimize or crash. A low-level hook sees the key before anyone
    // else, so we can swallow it while the game is in front.
    LRESULT CALLBACK keyboardHook(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HC_ACTION) {
            auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            if (info->vkCode == VK_SNAPSHOT && s_blockSystem && isGameFocused()) {
                // Trigger on release: some keyboards only ever send key up
                // for Print Screen.
                if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                    requestCapture();
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
        if (data.action == KeyboardInputData::Action::Release && !(s_hookActive && s_blockSystem)) {
            requestCapture();
        }
        return ListenerResult::Stop;
    }).leak();

    // While selecting an area, all mouse and keyboard input goes to the
    // overlay instead of the game.
    KeyboardInputEvent().listen([](KeyboardInputData& data) {
        auto overlay = SelectionOverlay::s_current;
        if (!overlay) return ListenerResult::Propagate;
        overlay->onKey(data);
        return ListenerResult::Stop;
    }).leak();

    MouseInputEvent().listen([](MouseInputData& data) {
        auto overlay = SelectionOverlay::s_current;
        if (!overlay) return ListenerResult::Propagate;
        overlay->onMouseButton(data);
        return ListenerResult::Stop;
    }).leak();
}
