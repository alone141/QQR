#include "ScreenPlayer.hpp"
#include "FrameLayout.hpp"
#include "FrameRenderer.hpp"
#include "Image.hpp"
#include "Manifest.hpp"
#include "Transmitter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

namespace vfdt
{

#ifdef _WIN32

namespace
{

constexpr std::size_t frames_ahead = 6;
constexpr wchar_t window_class_name[] = L"VfdtPlayer";

struct ReadyFrame
{
    std::uint64_t index{};
    int width{};
    int height{};
    std::vector<std::uint32_t> pixels;
};

// Renders frames ahead of the display on a worker thread.
class FrameQueue
{
public:
    FrameQueue(const Transmitter& transmitter, const FrameRenderer& renderer)
        : transmitter{transmitter}
        , renderer{renderer}
        , frames{}
        , stopping{false}
        , worker{[this]() { Produce(); }}
    {
    }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    ~FrameQueue()
    {
        {
            const std::lock_guard<std::mutex> lock{mutex};
            stopping = true;
        }
        changed.notify_all();
        worker.join();
    }

    ReadyFrame Take()
    {
        std::unique_lock<std::mutex> lock{mutex};
        changed.wait(lock, [this]() { return !frames.empty(); });
        ReadyFrame frame = std::move(frames.front());
        frames.pop_front();
        changed.notify_all();
        return frame;
    }

private:
    void Produce()
    {
        for (std::uint64_t index = 0;; ++index)
        {
            const Image image = renderer.Render(transmitter.FramePayload(index), static_cast<std::uint32_t>(index));
            ReadyFrame frame{index, image.width, image.height, std::vector<std::uint32_t>(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height))};
            for (std::size_t i = 0; i < frame.pixels.size(); ++i)
            {
                const std::uint8_t* rgb = image.pixels.data() + 3 * i;
                frame.pixels[i] = static_cast<std::uint32_t>(rgb[2]) | (static_cast<std::uint32_t>(rgb[1]) << 8) | (static_cast<std::uint32_t>(rgb[0]) << 16);
            }

            std::unique_lock<std::mutex> lock{mutex};
            changed.wait(lock, [this]() { return stopping || frames.size() < frames_ahead; });
            if (stopping)
            {
                return;
            }
            frames.push_back(std::move(frame));
            changed.notify_all();
        }
    }

    const Transmitter& transmitter;
    const FrameRenderer& renderer;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<ReadyFrame> frames;
    bool stopping;
    std::thread worker;
};

struct WindowState
{
    bool closing{};
    bool paused{true};
    const ReadyFrame* current{};
};

void Present(HWND window, const ReadyFrame& frame, bool clearFirst)
{
    RECT client{};
    GetClientRect(window, &client);
    HDC dc = GetDC(window);
    if (clearFirst)
    {
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frame.width;
    info.bmiHeader.biHeight = -frame.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int left = (client.right - frame.width) / 2;
    const int top = (client.bottom - frame.height) / 2;
    SetDIBitsToDevice(dc, left, top, static_cast<DWORD>(frame.width), static_cast<DWORD>(frame.height), 0, 0, 0,
                      static_cast<UINT>(frame.height), frame.pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(window, dc);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    WindowState* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message)
    {
    case WM_KEYDOWN:
        if (state != nullptr && wParam == VK_ESCAPE)
        {
            state->closing = true;
        }
        else if (state != nullptr && wParam == VK_SPACE)
        {
            state->paused = !state->paused;
        }
        return 0;
    case WM_CLOSE:
        if (state != nullptr)
        {
            state->closing = true;
        }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        if (state != nullptr && state->current != nullptr)
        {
            Present(window, *state->current, true);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void WaitForRefresh(int refreshRate)
{
    if (FAILED(DwmFlush()))
    {
        std::this_thread::sleep_for(std::chrono::microseconds{1000000 / std::max(1, refreshRate)});
    }
}

} // namespace

PlayCode PlayContent(std::span<const std::uint8_t> content, const PlayerConfig& config, PlayerStatus& status, const PlayerProgress& progress)
{
    // Without this Windows scales the window on high-DPI screens and blurs every cell.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return PlayCode::NoDisplay;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const int refreshRate = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                                ? static_cast<int>(mode.dmDisplayFrequency)
                                : 60;

    const bool windowed = config.windowWidth > 0 && config.windowHeight > 0;
    const RECT monitorRect = monitorInfo.rcMonitor;
    const int screenWidth = windowed ? config.windowWidth : monitorRect.right - monitorRect.left;
    const int screenHeight = windowed ? config.windowHeight : monitorRect.bottom - monitorRect.top;

    FrameLayout layout{};
    if (layout.Configure(GridCells(screenWidth, config.cellPixels), GridCells(screenHeight, config.cellPixels), config.bitsPerCell) != LayoutCode::Ok)
    {
        return PlayCode::BadLayout;
    }
    const Transmitter transmitter{content, SessionIdFor(content), layout, config.repairRatio};
    const FrameRenderer renderer{layout, config.cellPixels};

    status = PlayerStatus{};
    status.screenWidth = screenWidth;
    status.screenHeight = screenHeight;
    status.gridWidth = layout.GridWidth();
    status.gridHeight = layout.GridHeight();
    status.payloadBytes = layout.PayloadBytes();
    status.packetsPerFrame = layout.PacketsPerFrame();
    status.framesPerPass = transmitter.FramesPerPass();
    status.refreshRate = refreshRate;
    status.refreshesPerFrame = std::max(1, static_cast<int>(std::lround(refreshRate / config.displayFps)));
    status.framesPerSecond = static_cast<double>(refreshRate) / status.refreshesPerFrame;
    status.paused = true;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = window_class_name;
    RegisterClassW(&windowClass);

    RECT windowRect{0, 0, screenWidth, screenHeight};
    DWORD style = WS_POPUP;
    DWORD extendedStyle = WS_EX_TOPMOST;
    int left = monitorRect.left;
    int top = monitorRect.top;
    if (windowed)
    {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        extendedStyle = 0;
        AdjustWindowRectEx(&windowRect, style, FALSE, extendedStyle);
        left = CW_USEDEFAULT;
        top = CW_USEDEFAULT;
    }
    HWND window = CreateWindowExW(extendedStyle, window_class_name, L"vfdt player (Space: start/pause, Esc: quit)", style, left, top,
                                  windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, nullptr, nullptr, instance, nullptr);
    if (window == nullptr)
    {
        return PlayCode::NoDisplay;
    }
    WindowState state{};
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    if (!windowed)
    {
        ShowCursor(FALSE);
    }

    {
        FrameQueue queue{transmitter, renderer};
        ReadyFrame current = queue.Take();
        state.current = &current;
        Present(window, current, true);
        status.framesShown = 1;
        if (progress)
        {
            progress(status);
        }

        auto lastProgress = std::chrono::steady_clock::now();
        while (!state.closing)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (state.closing)
            {
                break;
            }

            status.paused = state.paused;
            if (state.paused)
            {
                WaitForRefresh(refreshRate);
            }
            else
            {
                current = queue.Take();
                Present(window, current, false);
                ++status.framesShown;
                for (int i = 0; i < status.refreshesPerFrame; ++i)
                {
                    WaitForRefresh(refreshRate);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (progress && now - lastProgress >= std::chrono::seconds{1})
            {
                lastProgress = now;
                progress(status);
            }
        }
        state.current = nullptr;
    }

    if (!windowed)
    {
        ShowCursor(TRUE);
    }
    DestroyWindow(window);
    UnregisterClassW(window_class_name, instance);
    return PlayCode::Ok;
}

#else

PlayCode PlayContent(std::span<const std::uint8_t>, const PlayerConfig&, PlayerStatus& status, const PlayerProgress&)
{
    status = PlayerStatus{};
    return PlayCode::Unsupported;
}

#endif

} // namespace vfdt
