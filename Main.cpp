#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <TlHelp32.h>
#include <tchar.h>
#include <d3d11.h>
#include <imgui/imgui.h>
#include <imgui/backend/imgui_impl_win32.h>
#include <imgui/backend/imgui_impl_dx11.h>
#pragma comment(lib, "d3d11.lib")

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Global variables
HANDLE hProcess = NULL;
HWND g_hwnd = NULL;
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
bool g_done = false;
bool g_show_menu = true;
float g_global_color[4] = { 0.00f, 0.00f, 0.00f, 1.00f };
std::vector<std::pair<std::string, std::pair<float, std::intptr_t>>> g_variables_list;
uintptr_t g_baseAddress = 0;

// Read from memory function
template <typename T>
T Read(uintptr_t address) {
    if (!hProcess || address == 0) return T();
    T buffer;
    ReadProcessMemory(hProcess, (LPCVOID)address, &buffer, sizeof(T), nullptr);
    return buffer;
}

// Custom function for reading Arma 3 game strings
std::string ReadGameString(uintptr_t address) {
    if (!hProcess || address == 0) return "";

    uint32_t size = Read<uint32_t>(address + 0x8);
    if (size <= 0 || size > 512) return "";

    std::vector<char> buffer(size + 1);
    ReadProcessMemory(hProcess, (LPCVOID)(address + 0x10), buffer.data(), size, nullptr);
    buffer[size] = '\0';
    return std::string(buffer.data());
}

std::vector<std::pair<std::string, std::pair<float, std::intptr_t>>> ScanMissionVariables() {
    std::vector<std::pair<std::string, std::pair<float, std::intptr_t>>> temp_list;

    if (!g_baseAddress) return temp_list;

    uintptr_t uWorldPtr = Read<uintptr_t>(g_baseAddress + 0x21C8850);
    if (!uWorldPtr) return temp_list;

    auto mission_namespace = Read<uint64_t>(uWorldPtr + 0x1750);
    if (!mission_namespace) return temp_list;

    auto curr_namespace = Read<uint64_t>(mission_namespace + 0x20);
    auto curr_namespace_size = Read<uint32_t>(mission_namespace + 0x28);

    for (uint32_t i = 0; i < curr_namespace_size; ++i) {
        auto curr_namespace_entry = Read<uintptr_t>(curr_namespace + (0x18 * i));
        if (!curr_namespace_entry) continue;

        auto entry_size = Read<uint32_t>(curr_namespace + (i * 0x18) + 0x8);

        for (uint32_t j = 0; j < entry_size; ++j) {
            uintptr_t gameVarAddr = curr_namespace_entry + (0x28 * j);
            uintptr_t namePtr = Read<uintptr_t>(gameVarAddr + 0x8);
            if (!namePtr) continue;

            std::string varName = ReadGameString(namePtr);
            if (varName.empty()) continue;

            uintptr_t typePtr = Read<uintptr_t>(gameVarAddr + 0x18);
            if (typePtr) {
                float value = Read<float>(typePtr + 0x18);
                temp_list.push_back({ varName, { value, (std::intptr_t)typePtr + 0x18 } });
            }
        }
    }
    return temp_list;
}

uintptr_t GetModuleBase(const wchar_t* moduleName, DWORD pid) {
    MODULEENTRY32 moduleEntry = { 0 };
    moduleEntry.dwSize = sizeof(moduleEntry);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    uintptr_t result = 0;
    if (Module32First(snapshot, &moduleEntry)) {
        do {
            if (wcscmp(moduleEntry.szModule, moduleName) == 0) {
                result = (uintptr_t)moduleEntry.modBaseAddr;
                break;
            }
        } while (Module32Next(snapshot, &moduleEntry));
    }

    CloseHandle(snapshot);
    return result;
}

bool ConnectToGame() {
    HWND hwnd = FindWindowA(nullptr, "Model Viewer");
    if (!hwnd) {
        hwnd = FindWindowA(nullptr, "Arma 3");
    }

    if (!hwnd) {
        MessageBoxA(nullptr, "Arma 3 no encontrado.", "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    hProcess = OpenProcess(PROCESS_VM_READ, FALSE, processId);

    if (!hProcess) {
        MessageBoxA(nullptr, "No se pudo abrir el proceso (¿Ejecutaste como admin?).", "Error", MB_OK | MB_ICONERROR);
        return false;
    }

    g_baseAddress = GetModuleBase(L"Arma3_x64.exe", processId);
    if (!g_baseAddress) {
        MessageBoxA(nullptr, "No se encontró arma3_x64.exe", "Error", MB_OK | MB_ICONERROR);
        CloseHandle(hProcess);
        hProcess = nullptr;
        return false;
    }

    return true;
}

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

// Win32 message handler
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProc(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };

    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK) {
        return false;
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void DrawMainMenu() {
    if (!g_show_menu) return;

    ImGui::Begin("Arma 3 Mission Variable Editor", &g_show_menu, ImGuiWindowFlags_MenuBar);

    // Menu Bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Conectar al Juego", "Ctrl+C")) {
            }
            if (ImGui::MenuItem("Escanear Variables", "Ctrl+E", nullptr, hProcess != nullptr)) {
                g_variables_list = ScanMissionVariables();
            }
            if (ImGui::MenuItem("Limpiar Lista", "Ctrl+L")) {
                g_variables_list.clear();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Salir", "Alt+F4")) {
                g_done = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    // Status
    //ImGui::Text("Estado: %s", hProcess ? "Conectado" : "No conectado");

    if (hProcess) {
		ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Conectado");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No conectado");
    }

    ImGui::Separator();

    // Configuration
    ImGui::Text("Configurar fondo:");
    ImGui::ColorEdit4("Color", g_global_color);

    ImGui::Separator();

    // Variables Table
    ImGui::Text("Variables: %d", (int)g_variables_list.size());

    if (ImGui::BeginChild("ScrollingRegion", ImVec2(0, 0), true)) {
        if (ImGui::BeginTable("VarsTable", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable)) {

            ImGui::TableSetupColumn("Nombre", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Valor", ImGuiTableColumnFlags_WidthFixed, 200);
            ImGui::TableHeadersRow();

            for (const auto& var : g_variables_list) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", var.first.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", var.second.first);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

int main() {
    // Create application window
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr),
                      nullptr, nullptr, nullptr, nullptr, _T("Arma3 Variable Editor"), nullptr };
    ::RegisterClassEx(&wc);

    HWND hwnd = ::CreateWindow(wc.lpszClassName, _T("Arma 3 Variable Editor"),
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
        nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        MessageBoxA(nullptr, "Error inicializando Direct3D", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Main loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    // Try to auto-connect to Arma 3
    ConnectToGame();

    while (!g_done) {
        // Poll and handle messages
        if (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_done = true;
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Draw our menu
        DrawMainMenu();

        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_C)) { // Ctrl+C
                ConnectToGame();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E)) { // Ctrl+E
                g_variables_list = ScanMissionVariables();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_L)) { // Ctrl+L
                g_variables_list.clear();       
            }
        }

        // Rendering
        ImGui::Render();

        const float clear_color_with_alpha[4] = { g_global_color[0] * g_global_color[3],
                                                  g_global_color[1] * g_global_color[3],
                                                  g_global_color[2] * g_global_color[3],
                                                  g_global_color[3] };

        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // Present with vsync
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClass(wc.lpszClassName, wc.hInstance);

    // Close process handle if open
    if (hProcess) {
        CloseHandle(hProcess);
    }

    return 0;
}