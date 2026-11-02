#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM word, LPARAM value);

namespace {

using clock_type = std::chrono::steady_clock;
constexpr int width = 640;
constexpr int height = 3072;
constexpr int row_count = 200;
constexpr int warmup_frames = 32;
constexpr std::size_t sample_count = 256;
constexpr wchar_t class_name[] = L"MetaLooperImGuiFrameBench";

[[noreturn]] void fail_hresult(char const* operation, HRESULT result) {
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer), "%s: HRESULT=0x%08lX", operation,
                  static_cast<unsigned long>(result));
    throw std::runtime_error(buffer);
}

void check_hresult(char const* operation, HRESULT result) {
    if (FAILED(result)) {
        fail_hresult(operation, result);
    }
}

[[noreturn]] void fail_win32(char const* operation) {
    DWORD const error = GetLastError();
    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer), "%s: Win32=%lu, HRESULT=0x%08lX",
                  operation, static_cast<unsigned long>(error),
                  static_cast<unsigned long>(HRESULT_FROM_WIN32(error)));
    throw std::runtime_error(buffer);
}

template <class Interface>
struct release_interface {
    void operator()(Interface* pointer) const noexcept {
        if (pointer != nullptr) {
            pointer->Release();
        }
    }
};

template <class Interface>
using com_owner = std::unique_ptr<Interface, release_interface<Interface>>;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM word, LPARAM value) {
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui_ImplWin32_WndProcHandler(window, message, word, value)) {
        return 1;
    }
    return DefWindowProcW(window, message, word, value);
}

struct frame_bench {
    HINSTANCE instance = nullptr;
    ATOM window_class = 0;
    HWND window = nullptr;
    ImGuiContext* imgui_context = nullptr;
    bool platform_ready = false;
    bool renderer_ready = false;
    bool warp = false;
    D3D_FEATURE_LEVEL feature_level{};
    com_owner<ID3D11Device> device;
    com_owner<ID3D11DeviceContext> context;
    com_owner<IDXGISwapChain> swap_chain;
    com_owner<ID3D11RenderTargetView> render_target;
    com_owner<ID3D11Query> completion;
    std::array<std::array<char, 80>, row_count> rows{};
    unsigned long long occluded_presents = 0;

    frame_bench() = default;
    frame_bench(frame_bench const&) = delete;
    frame_bench& operator=(frame_bench const&) = delete;

    ~frame_bench() {
        shutdown();
    }

    bool shutdown() noexcept {
        if (renderer_ready) {
            ImGui_ImplDX11_Shutdown();
            renderer_ready = false;
        }
        if (platform_ready) {
            ImGui_ImplWin32_Shutdown();
            platform_ready = false;
        }
        if (imgui_context != nullptr) {
            ImGui::DestroyContext(imgui_context);
            imgui_context = nullptr;
        }
        if (context) {
            context->ClearState();
            context->Flush();
        }
        completion.reset();
        render_target.reset();
        swap_chain.reset();
        context.reset();
        device.reset();
        bool success = true;
        if (window != nullptr) {
            if (!DestroyWindow(window)) {
                std::fprintf(stderr, "DestroyWindow: Win32=%lu\n", GetLastError());
                success = false;
            }
            window = nullptr;
        }
        if (window_class != 0) {
            if (!UnregisterClassW(class_name, instance)) {
                std::fprintf(stderr, "UnregisterClassW: Win32=%lu\n", GetLastError());
                success = false;
            }
            window_class = 0;
        }
        return success;
    }

    HRESULT create_device(D3D_DRIVER_TYPE driver, DXGI_SWAP_CHAIN_DESC& description) {
        ID3D11Device* new_device = nullptr;
        ID3D11DeviceContext* new_context = nullptr;
        IDXGISwapChain* new_swap_chain = nullptr;
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT const result = D3D11CreateDeviceAndSwapChain(
            nullptr, driver, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
            &description, &new_swap_chain, &new_device, &feature_level, &new_context);
        device.reset(new_device);
        context.reset(new_context);
        swap_chain.reset(new_swap_chain);
        if (FAILED(result)) {
            swap_chain.reset();
            context.reset();
            device.reset();
        }
        return result;
    }

    void initialize() {
        instance = GetModuleHandleW(nullptr);
        if (instance == nullptr) {
            fail_win32("GetModuleHandleW");
        }
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = window_proc;
        description.hInstance = instance;
        description.lpszClassName = class_name;
        window_class = RegisterClassExW(&description);
        if (window_class == 0) {
            fail_win32("RegisterClassExW");
        }
        window = CreateWindowExW(0, class_name, L"ImGui frame benchmark",
                                 WS_POPUP, 0, 0, width, height,
                                 nullptr, nullptr, instance, nullptr);
        if (window == nullptr) {
            fail_win32("CreateWindowExW");
        }
        if (IsWindowVisible(window)) {
            throw std::runtime_error("Benchmark HWND must remain hidden");
        }
        DXGI_SWAP_CHAIN_DESC swap_description{};
        swap_description.BufferDesc.Width = width;
        swap_description.BufferDesc.Height = height;
        swap_description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_description.SampleDesc.Count = 1;
        swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_description.BufferCount = 2;
        swap_description.OutputWindow = window;
        swap_description.Windowed = TRUE;
        swap_description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        HRESULT result = create_device(D3D_DRIVER_TYPE_HARDWARE, swap_description);
        if (FAILED(result)) {
            std::fprintf(stderr, "Hardware creation failed: HRESULT=0x%08lX; trying WARP\n",
                         static_cast<unsigned long>(result));
            warp = true;
            result = create_device(D3D_DRIVER_TYPE_WARP, swap_description);
        }
        check_hresult("D3D11CreateDeviceAndSwapChain", result);
        ID3D11Texture2D* raw_buffer = nullptr;
        result = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(&raw_buffer));
        com_owner<ID3D11Texture2D> back_buffer(raw_buffer);
        check_hresult("IDXGISwapChain::GetBuffer", result);
        ID3D11RenderTargetView* raw_target = nullptr;
        result = device->CreateRenderTargetView(back_buffer.get(), nullptr, &raw_target);
        render_target.reset(raw_target);
        check_hresult("CreateRenderTargetView", result);
        D3D11_QUERY_DESC query_description{};
        query_description.Query = D3D11_QUERY_EVENT;
        ID3D11Query* raw_query = nullptr;
        result = device->CreateQuery(&query_description, &raw_query);
        completion.reset(raw_query);
        check_hresult("CreateQuery(EVENT)", result);
        if (!IMGUI_CHECKVERSION()) {
            throw std::runtime_error("ImGui version/layout mismatch");
        }
        imgui_context = ImGui::CreateContext();
        if (imgui_context == nullptr) {
            throw std::runtime_error("ImGui::CreateContext failed");
        }
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui::GetStyle().ItemSpacing = ImVec2(4.0f, 0.0f);
        platform_ready = ImGui_ImplWin32_Init(window);
        if (!platform_ready) {
            throw std::runtime_error("ImGui_ImplWin32_Init returned false (no HRESULT API)");
        }
        renderer_ready = ImGui_ImplDX11_Init(device.get(), context.get());
        if (!renderer_ready) {
            throw std::runtime_error("ImGui_ImplDX11_Init returned false (no HRESULT API)");
        }
        if (!ImGui_ImplDX11_CreateDeviceObjects()) {
            throw std::runtime_error("ImGui_ImplDX11_CreateDeviceObjects returned false (backend hides HRESULT)");
        }
        if (!io.Fonts->IsBuilt() || io.Fonts->TexID == 0) {
            throw std::runtime_error("Font atlas or DX11 font texture is missing");
        }
        for (int index = 0; index < row_count; ++index) {
            std::snprintf(rows[index].data(), rows[index].size(),
                          "Row %03d: Dear ImGui frame benchmark payload", index);
        }
        build_frame();
        validate_draw_data();
        submit_frame();
        drain_gpu();
    }

    void pump_messages() {
        MSG message{};
        for (int count = 0; count < 256; ++count) {
            if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                return;
            }
            if (message.message == WM_QUIT) {
                throw std::runtime_error("Unexpected WM_QUIT");
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        throw std::runtime_error("Message pump exceeded bounded batch");
    }

    void build_frame() {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width), static_cast<float>(height)));
        bool const visible = ImGui::Begin("Frame workload", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        if (visible) {
            for (auto const& row : rows) {
                ImGui::TextUnformatted(row.data());
            }
        }
        ImGui::End();
        ImGui::Render();
        if (!visible) {
            throw std::runtime_error("ImGui workload window was skipped");
        }
    }

    void validate_draw_data() const {
        ImDrawData const* data = ImGui::GetDrawData();
        if (data == nullptr || !data->Valid || data->CmdListsCount == 0 ||
            data->TotalVtxCount < row_count * 4 || data->TotalIdxCount == 0 ||
            data->DisplaySize.x != width || data->DisplaySize.y != height) {
            throw std::runtime_error("Missing, empty or incorrectly sized ImGui draw data");
        }
    }

    HRESULT submit_frame() {
        ID3D11RenderTargetView* target = render_target.get();
        constexpr float clear_color[] = {0.08f, 0.08f, 0.08f, 1.0f};
        context->OMSetRenderTargets(1, &target, nullptr);
        context->ClearRenderTargetView(target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT const result = swap_chain->Present(0, 0);
        check_hresult("Present(0,0)", result);
        if (result == DXGI_STATUS_OCCLUDED) {
            ++occluded_presents;
        } else if (result != S_OK) {
            fail_hresult("Unexpected Present status", result);
        }
        return result;
    }

    void drain_gpu() {
        context->End(completion.get());
        context->Flush();
        auto const deadline = clock_type::now() + std::chrono::seconds(5);
        for (;;) {
            BOOL completed = FALSE;
            HRESULT const result = context->GetData(completion.get(), &completed,
                                                    sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            check_hresult("GetData(EVENT)", result);
            if (result == S_OK && completed) {
                break;
            }
            if (clock_type::now() >= deadline) {
                check_hresult("GetDeviceRemovedReason", device->GetDeviceRemovedReason());
                fail_hresult("GPU drain timed out after 5 seconds", HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            }
            std::this_thread::yield();
        }
        check_hresult("GetDeviceRemovedReason", device->GetDeviceRemovedReason());
    }
};

double elapsed_us(clock_type::time_point begin, clock_type::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

void report(char const* label, std::array<double, sample_count> samples) {
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (double value : samples) {
        sum += value;
    }
    std::printf("%-24s mean=%10.3f us/frame  median=%10.3f  p95=%10.3f  min=%10.3f\n",
                label, sum / samples.size(),
                (samples[sample_count / 2 - 1] + samples[sample_count / 2]) / 2.0,
                samples[(sample_count * 95 + 99) / 100 - 1], samples.front());
}

}

int main() {
    try {
        auto const initialization_begin = clock_type::now();
        frame_bench bench;
        bench.initialize();
        auto const initialization_end = clock_type::now();
        for (int frame = 0; frame < warmup_frames; ++frame) {
            bench.pump_messages();
            bench.build_frame();
            bench.validate_draw_data();
            bench.submit_frame();
        }
        bench.drain_gpu();
        bench.occluded_presents = 0;
        std::array<double, sample_count> cpu_samples{};
        std::array<double, sample_count> submission_samples{};
        for (std::size_t frame = 0; frame < sample_count; ++frame) {
            bench.pump_messages();
            auto const cpu_begin = clock_type::now();
            bench.build_frame();
            auto const cpu_end = clock_type::now();
            bench.validate_draw_data();
            auto const submission_begin = clock_type::now();
            bench.submit_frame();
            auto const submission_end = clock_type::now();
            cpu_samples[frame] = elapsed_us(cpu_begin, cpu_end);
            submission_samples[frame] = elapsed_us(submission_begin, submission_end);
            check_hresult("GetDeviceRemovedReason", bench.device->GetDeviceRemovedReason());
        }
        bench.drain_gpu();
        std::printf("Dear ImGui %s | Win32/DX11 | %s | feature level 0x%X\n",
                    ImGui::GetVersion(), bench.warp ? "WARP" : "hardware",
                    static_cast<unsigned>(bench.feature_level));
        std::printf("Hidden real HWND, %dx%d, %d TextUnformatted rows; warmup=%d, samples=%zu\n",
                    width, height, row_count, warmup_frames, sample_count);
        std::printf("Initialization (one observation): %.3f ms\n",
                    elapsed_us(initialization_begin, initialization_end) / 1000.0);
        std::puts("  Includes owner/row storage setup, module lookup, class registration, hidden HWND,");
        std::puts("  hardware device attempt (+ WARP retry if needed), swap chain, RTV, event query,");
        std::puts("  ImGui context/backends, shaders/font atlas, row formatting, first frame build,");
        std::puts("  validation, clear/render/Present(0,0), GPU drain (5-second timeout).");
        std::puts("  Excludes process/DLL startup, later warmup, steady samples, reporting and teardown.");
        report("CPU frame build", cpu_samples);
        report("GPU submission (CPU time)", submission_samples);
        std::puts("CPU scope: DX11/Win32 NewFrame + NewFrame/Begin + 200 TextUnformatted + End/Render.");
        std::puts("Submission scope: OMSetRenderTargets + ClearRenderTargetView + RenderDrawData + Present(0,0).");
        std::puts("Message pumping, draw-data checks, device checks and final GPU drain are outside frame timers.");
        std::printf("Last frame: vertices=%d, indices=%d; measured occluded presents=%llu/%zu\n",
                    ImGui::GetDrawData()->TotalVtxCount, ImGui::GetDrawData()->TotalIdxCount,
                    bench.occluded_presents, sample_count);
        std::puts("Hidden HWND may return DXGI_STATUS_OCCLUDED (success, counted); no visible presentation.");
        std::puts("Submission is CPU wall time, includes driver/backpressure, NOT GPU execution/completion time.");
        std::puts("Not equivalent to function-call chains or GDI window create/destroy lifecycle benchmarks.");
        std::puts("Backend void calls cannot expose all internal allocation/Map HRESULT failures.");
        auto const teardown_begin = clock_type::now();
        if (!bench.shutdown()) {
            throw std::runtime_error("Win32 teardown failed");
        }
        std::printf("Teardown (separate): %.3f ms; all owned ImGui/COM/HWND/class resources released.\n",
                    elapsed_us(teardown_begin, clock_type::now()) / 1000.0);
        return EXIT_SUCCESS;
    } catch (std::exception const& error) {
        std::fprintf(stderr, "imgui_frame_bench: %s\n", error.what());
        return EXIT_FAILURE;
    }
}