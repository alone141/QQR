#include "ScreenPlayer.hpp"
#include "FrameLayout.hpp"
#include "FrameRenderer.hpp"
#include "Image.hpp"
#include "Manifest.hpp"
#include "Transmitter.hpp"

#include <algorithm>
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
    explicit FrameQueue(const FrameSource& source)
        : source{source}
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
            const Image image = source(index);
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

    const FrameSource& source;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<ReadyFrame> frames;
    bool stopping;
    std::thread worker;
};

// The pass counter sits in the bottom quiet zone, centred, well away from the corner finders.
struct PassLabel
{
    HFONT font{};
    int height{};
    wchar_t text[32]{};
};

struct WindowState
{
    bool closing{};
    bool paused{true};
    const ReadyFrame* current{};
    const PassLabel* label{};
};

constexpr int min_label_pixels = 8;

void UpdateLabel(PassLabel& label, const PlayerStatus& status)
{
    const double pass = static_cast<double>(status.framesShown) / static_cast<double>(std::max<std::uint64_t>(1, status.framesPerPass));
    swprintf_s(label.text, L"pass %.2f", pass);
}

void DrawLabel(HDC dc, int left, int top, const ReadyFrame& frame, const PassLabel& label)
{
    RECT box{left, top + frame.height - label.height, left + frame.width, top + frame.height};
    const HGDIOBJ previous = SelectObject(dc, label.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(96, 96, 96));
    DrawTextW(dc, label.text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous);
}

void Present(HWND window, const ReadyFrame& frame, bool clearFirst, const PassLabel* label)
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
    if (label != nullptr)
    {
        DrawLabel(dc, left, top, frame, *label);
    }
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
            Present(window, *state->current, true, state->label);
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

bool IsWindowed(const PlayerConfig& config)
{
    return config.windowWidth > 0 && config.windowHeight > 0;
}

} // namespace

PlayCode QueryScreen(const PlayerConfig& config, ScreenInfo& out)
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
    out.refreshRate = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                          ? static_cast<int>(mode.dmDisplayFrequency)
                          : 60;
    const RECT monitorRect = monitorInfo.rcMonitor;
    out.width = IsWindowed(config) ? config.windowWidth : monitorRect.right - monitorRect.left;
    out.height = IsWindowed(config) ? config.windowHeight : monitorRect.bottom - monitorRect.top;
    return PlayCode::Ok;
}

PlayCode PlayFrames(const FrameSource& source, const PlayerConfig& config, PlayerStatus& status, const PlayerProgress& progress)
{
    ScreenInfo screen{};
    if (QueryScreen(config, screen) != PlayCode::Ok)
    {
        return PlayCode::NoDisplay;
    }
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    const RECT monitorRect = monitorInfo.rcMonitor;
    const bool windowed = IsWindowed(config);

    status.screenWidth = screen.width;
    status.screenHeight = screen.height;
    status.refreshRate = screen.refreshRate;
    status.refreshesPerFrame = std::max(1, static_cast<int>(std::lround(screen.refreshRate / config.displayFps)));
    status.framesPerSecond = static_cast<double>(screen.refreshRate) / status.refreshesPerFrame;
    status.framesShown = 0;
    status.paused = true;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = window_class_name;
    RegisterClassW(&windowClass);

    RECT windowRect{0, 0, screen.width, screen.height};
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
    PassLabel label{};
    label.height = quiet_zone_cells * config.cellPixels;
    if (status.framesPerPass > 0 && label.height >= min_label_pixels)
    {
        label.font = CreateFontW(label.height - 2, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    const PassLabel* shownLabel = label.font != nullptr ? &label : nullptr;
    state.label = shownLabel;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    if (!windowed)
    {
        ShowCursor(FALSE);
    }

    {
        FrameQueue queue{source};
        ReadyFrame current = queue.Take();
        state.current = &current;
        status.framesShown = 1;
        UpdateLabel(label, status);
        Present(window, current, true, shownLabel);
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
                WaitForRefresh(screen.refreshRate);
            }
            else
            {
                ReadyFrame next = queue.Take();
                // A smaller frame would leave the edges of a larger one behind.
                const bool resized = next.width != current.width || next.height != current.height;
                current = std::move(next);
                ++status.framesShown;
                UpdateLabel(label, status);
                Present(window, current, resized, shownLabel);
                for (int i = 0; i < status.refreshesPerFrame; ++i)
                {
                    WaitForRefresh(screen.refreshRate);
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
    if (label.font != nullptr)
    {
        DeleteObject(label.font);
    }
    UnregisterClassW(window_class_name, instance);
    return PlayCode::Ok;
}

PlayCode PlayContent(std::span<const std::uint8_t> content, const PlayerConfig& config, PlayerStatus& status, const PlayerProgress& progress)
{
    ScreenInfo screen{};
    if (QueryScreen(config, screen) != PlayCode::Ok)
    {
        return PlayCode::NoDisplay;
    }
    FrameLayout layout{};
    if (layout.Configure(GridCells(screen.width, config.cellPixels), GridCells(screen.height, config.cellPixels), config.bitsPerCell) != LayoutCode::Ok)
    {
        return PlayCode::BadLayout;
    }
    const Transmitter transmitter{content, SessionIdFor(content), layout, config.repairRatio};
    const FrameRenderer renderer{layout, config.cellPixels};
    const FrameSource source = [&transmitter, &renderer](std::uint64_t index)
    {
        return renderer.Render(transmitter.FramePayload(index), static_cast<std::uint32_t>(index));
    };

    status = PlayerStatus{};
    status.gridWidth = layout.GridWidth();
    status.gridHeight = layout.GridHeight();
    status.payloadBytes = layout.PayloadBytes();
    status.packetsPerFrame = layout.PacketsPerFrame();
    status.framesPerPass = transmitter.FramesPerPass();
    return PlayFrames(source, config, status, progress);
}

#else

PlayCode QueryScreen(const PlayerConfig&, ScreenInfo& out)
{
    out = ScreenInfo{};
    return PlayCode::Unsupported;
}

PlayCode PlayFrames(const FrameSource&, const PlayerConfig&, PlayerStatus& status, const PlayerProgress&)
{
    status.framesShown = 0;
    return PlayCode::Unsupported;
}

PlayCode PlayContent(std::span<const std::uint8_t>, const PlayerConfig&, PlayerStatus& status, const PlayerProgress&)
{
    status = PlayerStatus{};
    return PlayCode::Unsupported;
}

#endif

} // namespace vfdt
