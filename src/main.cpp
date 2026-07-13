#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>

#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <deque>
#include <condition_variable>

using namespace geode::prelude;

static bool g_showHitboxes = false;
static bool s_isDead = false;
static GameObject* s_collisionObject = nullptr;

struct OverlaySettings {
    float bgAlpha = 0.0f;
    bool fillHitboxes = false;
    float fillAlpha = 0.3f;
    int targetFPS = 0; 
    bool streamproof = true;
    bool hitboxTrail = false;
    int trailLength = 10;
};

static std::mutex g_settingsMutex;
static OverlaySettings g_currentSettings;

struct PlayerTrailState {
    cocos2d::CCRect rect1;
    cocos2d::CCRect rect2;
    std::vector<cocos2d::CCPoint> rotPts;
    bool hasRot;
};
static std::deque<PlayerTrailState> g_p1Trail;
static std::deque<PlayerTrailState> g_p2Trail;

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <emmintrin.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

enum class ShapeType { Rect, Polygon, Circle };

struct Vec2i { 
    int x; 
    int y; 
};

struct RenderShape {
    ShapeType type;
    RECT rect;                 
    std::vector<Vec2i> points; 
    Vec2i center;              
    float radius;              
    ImU32 color;                  
};

class RenderBufferPool {
public:
    RenderBufferPool(size_t capacity = 1500) : m_capacity(capacity) {
        m_buffer1.reserve(capacity);
        m_buffer2.reserve(capacity);
    }
    
    std::vector<RenderShape>* getWriteBuffer() {
        return m_writeBuffer;
    }
    
    void swapWriteBuffer() {
        std::lock_guard<std::mutex> lock(m_swapMutex);
        std::swap(m_writeBuffer, m_readBuffer);
    }
    
    std::vector<RenderShape>* getReadBuffer() {
        std::lock_guard<std::mutex> lock(m_swapMutex);
        return m_readBuffer;
    }
    
private:
    std::vector<RenderShape> m_buffer1;
    std::vector<RenderShape> m_buffer2;
    
    std::vector<RenderShape>* m_writeBuffer = &m_buffer1;  
    std::vector<RenderShape>* m_readBuffer = &m_buffer2;   
    
    std::mutex m_swapMutex;  
    size_t m_capacity;
};

static RenderBufferPool g_bufferPool;
static std::atomic<bool> g_overlayRunning{false};

static HWND g_overlayHwnd = nullptr;
static RECT g_lastWindowRect = {-1, -1, -1, -1}; 

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain1* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static IDCompositionDevice* g_dcompDevice = nullptr;
static IDCompositionTarget* g_dcompTarget = nullptr;
static IDCompositionVisual* g_dcompVisual = nullptr;

static std::atomic<bool> g_newDataReady{false};

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
        return true;
    if (uMsg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd) {
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[ 2 ] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    if (D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    IDXGIDevice* dxgiDevice = nullptr;
    if (g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)) != S_OK)
        return false;

    IDXGIFactory2* dxgiFactory = nullptr;
    if (CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory)) != S_OK) {
        dxgiDevice->Release();
        return false;
    }

    RECT rc;
    GetClientRect(hWnd, &rc);

    // Composition swapchain with premultiplied alpha: this is what makes the
    // window actually transparent (LWA_COLORKEY is ignored by flip-model presentation)
    DXGI_SWAP_CHAIN_DESC1 sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.Width = rc.right - rc.left;
    sd.Height = rc.bottom - rc.top;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = dxgiFactory->CreateSwapChainForComposition(dxgiDevice, &sd, nullptr, &g_pSwapChain);
    dxgiFactory->Release();
    if (hr != S_OK) {
        dxgiDevice->Release();
        return false;
    }

    hr = DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&g_dcompDevice));
    dxgiDevice->Release();
    if (hr != S_OK)
        return false;

    if (g_dcompDevice->CreateTargetForHwnd(hWnd, TRUE, &g_dcompTarget) != S_OK)
        return false;
    if (g_dcompDevice->CreateVisual(&g_dcompVisual) != S_OK)
        return false;

    g_dcompVisual->SetContent(g_pSwapChain);
    g_dcompTarget->SetRoot(g_dcompVisual);
    g_dcompDevice->Commit();

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    return true;
}

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

void initExternalOverlay() {
    if (g_overlayHwnd) return;

    HINSTANCE hInstance = GetModuleHandle(nullptr);
    WNDCLASS wc = {};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "HitboxOverlayClass";
    wc.hbrBackground = nullptr;
    RegisterClass(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    g_overlayHwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, "Hitboxes", WS_POPUP,
        0, 0, screenW, screenH,
        nullptr, nullptr, hInstance, nullptr
    );

    bool useStreamproof = false;
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        useStreamproof = g_currentSettings.streamproof;
    }

    if (g_overlayHwnd && useStreamproof) {
        SetWindowDisplayAffinity(g_overlayHwnd, WDA_EXCLUDEFROMCAPTURE);
    }

    // Per-pixel transparency comes from the DComp swapchain; the layered style
    // only remains for WS_EX_TRANSPARENT click-through
    SetLayeredWindowAttributes(g_overlayHwnd, 0, 255, LWA_ALPHA);
    ShowWindow(g_overlayHwnd, SW_SHOWNA);
}

void renderOverlayThread() {
    initExternalOverlay(); 
    if (!g_overlayHwnd || !CreateDeviceD3D(g_overlayHwnd)) return;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_overlayHwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    while (g_overlayRunning) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_overlayRunning = false;
            }
        }

        if (!g_overlayRunning) break; 

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        OverlaySettings frameSettings;
        {
            std::lock_guard<std::mutex> lock(g_settingsMutex);
            frameSettings = g_currentSettings;
        }

        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
        auto* shapeBuffer = g_bufferPool.getReadBuffer();
        
        if (shapeBuffer) {
            for (const auto& shape : *shapeBuffer) {
                
                ImVec4 colorVec = ImGui::ColorConvertU32ToFloat4(shape.color);
                ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(ImVec4(colorVec.x, colorVec.y, colorVec.z, colorVec.w * frameSettings.fillAlpha));

                if (shape.type == ShapeType::Rect) {
                    ImVec2 minPt = ImVec2(static_cast<float>(shape.rect.left), static_cast<float>(shape.rect.top));
                    ImVec2 maxPt = ImVec2(static_cast<float>(shape.rect.right), static_cast<float>(shape.rect.bottom));
                    
                    if (frameSettings.fillHitboxes) {
                        draw_list->AddRectFilled(minPt, maxPt, fillColor);
                    }
                    draw_list->AddRect(minPt, maxPt, shape.color, 0.0f, 0, 1.5f);
                } 
                else if (shape.type == ShapeType::Polygon && shape.points.size() >= 3) {
                    const Vec2i* pts = shape.points.data();

                    if (shape.points.size() == 3) {
                        ImVec2 p1 = ImVec2(static_cast<float>(pts[ 0 ].x), static_cast<float>(pts[ 0 ].y));
                        ImVec2 p2 = ImVec2(static_cast<float>(pts[ 1 ].x), static_cast<float>(pts[ 1 ].y));
                        ImVec2 p3 = ImVec2(static_cast<float>(pts[ 2 ].x), static_cast<float>(pts[ 2 ].y));
                        
                        if (frameSettings.fillHitboxes) {
                            draw_list->AddTriangleFilled(p1, p2, p3, fillColor);
                        }
                        draw_list->AddTriangle(p1, p2, p3, shape.color, 1.5f);
                    }
                    else if (shape.points.size() == 4) {
                        ImVec2 p1 = ImVec2(static_cast<float>(pts[ 0 ].x), static_cast<float>(pts[ 0 ].y));
                        ImVec2 p2 = ImVec2(static_cast<float>(pts[ 1 ].x), static_cast<float>(pts[ 1 ].y));
                        ImVec2 p3 = ImVec2(static_cast<float>(pts[ 2 ].x), static_cast<float>(pts[ 2 ].y));
                        ImVec2 p4 = ImVec2(static_cast<float>(pts[ 3 ].x), static_cast<float>(pts[ 3 ].y));
                        
                        if (frameSettings.fillHitboxes) {
                            draw_list->AddQuadFilled(p1, p2, p3, p4, fillColor);
                        }
                        draw_list->AddQuad(p1, p2, p3, p4, shape.color, 1.5f);
                    }
                }
                else if (shape.type == ShapeType::Circle) {
                    ImVec2 centerPt = ImVec2(static_cast<float>(shape.center.x), static_cast<float>(shape.center.y));
                    
                    if (frameSettings.fillHitboxes) {
                        draw_list->AddCircleFilled(centerPt, shape.radius, fillColor, 32);
                    }
                    draw_list->AddCircle(centerPt, shape.radius, shape.color, 32, 1.5f);
                }
            }
        }

        ImGui::Render();
        const float clear_color_with_alpha[ 4 ] = { 0.0f, 0.0f, 0.0f, frameSettings.bgAlpha };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        
        g_pSwapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);

        if (frameSettings.targetFPS == 0) {
            while (!g_newDataReady.load(std::memory_order_acquire) && g_overlayRunning) {
                MSG msg;
                while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                    ::TranslateMessage(&msg);
                    ::DispatchMessage(&msg);
                    if (msg.message == WM_QUIT) g_overlayRunning = false;
                }
                std::this_thread::yield();
            }
            g_newDataReady.store(false, std::memory_order_release); 
        } 
        else {
            static auto nextFrameTime = std::chrono::high_resolution_clock::now();
            auto targetDuration = std::chrono::duration<double, std::milli>(1000.0 / frameSettings.targetFPS);
            nextFrameTime += std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(targetDuration);
            
            auto now = std::chrono::high_resolution_clock::now();
            if (nextFrameTime < now) {
                nextFrameTime = now;
            } else {
                while (std::chrono::high_resolution_clock::now() < nextFrameTime) {
                    std::this_thread::yield(); 
                }
            }
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    if (g_dcompVisual) { g_dcompVisual->Release(); g_dcompVisual = nullptr; }
    if (g_dcompTarget) { g_dcompTarget->Release(); g_dcompTarget = nullptr; }
    if (g_dcompDevice) { g_dcompDevice->Release(); g_dcompDevice = nullptr; }
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }

    if (g_overlayHwnd) {
        DestroyWindow(g_overlayHwnd);
        g_overlayHwnd = nullptr;
        UnregisterClass("HitboxOverlayClass", GetModuleHandle(nullptr));
    }
}
#endif

static void forEachObject(GJBaseGameLayer const* game, auto&& callback) {
    int count = game->m_sections.empty() ? -1 : game->m_sections.size();
    for (int i = game->m_leftSectionIndex; i <= game->m_rightSectionIndex && i < count; ++i) {
        auto leftSection = game->m_sections[ i ];
        if (!leftSection) continue;
        auto leftSectionSize = leftSection->size();
        for (int j = game->m_bottomSectionIndex; j <= game->m_topSectionIndex && j < leftSectionSize; ++j) {
            auto section = leftSection->at(j);
            if (!section) continue;
            auto sectionSize = game->m_sectionSizes[ i ]->at(j);
            for (int k = 0; k < sectionSize; ++k) {
                auto obj = section->at(k);
                if (!obj) continue;
                callback(obj);
            }
        }
    }
}

class $modify(ShowHitboxesGJBGLHook, GJBaseGameLayer) {
    void visitHitboxes() {
#ifdef GEODE_IS_WINDOWS
        auto mod = Mod::get();
        float currentBgOpacity = mod->getSettingValue<double>("bg-opacity");
        bool currentFill = mod->getSettingValue<bool>("fill-hitboxes");
        float currentFillOpacity = mod->getSettingValue<double>("fill-opacity");
        bool matchTick = mod->getSettingValue<bool>("match-tick");
        int fpsCap = mod->getSettingValue<int64_t>("fps-cap");
        bool currentStreamproof = mod->getSettingValue<bool>("streamproof");
        bool currentHitboxTrail = mod->getSettingValue<bool>("hitbox-trail");
        int currentTrailLength = mod->getSettingValue<int64_t>("trail-length");

        {
            std::lock_guard<std::mutex> lock(g_settingsMutex);
            g_currentSettings.bgAlpha = currentBgOpacity;
            g_currentSettings.fillHitboxes = currentFill;
            g_currentSettings.fillAlpha = currentFillOpacity;
            g_currentSettings.targetFPS = matchTick ? 0 : fpsCap;
            g_currentSettings.streamproof = currentStreamproof;
            g_currentSettings.hitboxTrail = currentHitboxTrail;
            g_currentSettings.trailLength = currentTrailLength;
        }

        static HWND gdHwnd = nullptr;
        if (!gdHwnd) {
            gdHwnd = FindWindowA(NULL, "Geometry Dash");
            if (!gdHwnd) return;
        }

        static bool tKeyPressed = false;
        static bool threadStarted = false;

        if (GetAsyncKeyState('T') & 0x8000) {
            if (!tKeyPressed) {
                g_showHitboxes = !g_showHitboxes;
                
                if (!threadStarted) {
                    g_overlayRunning = true;
                    std::thread(renderOverlayThread).detach();
                    threadStarted = true;
                }
                
                if (g_overlayHwnd) {
                    ShowWindow(g_overlayHwnd, g_showHitboxes ? SW_SHOWNA : SW_HIDE);
                }

                tKeyPressed = true;
            }
        } else {
            tKeyPressed = false;
        }

        if (!g_showHitboxes || m_isEditor) {
            if (threadStarted && g_currentSettings.targetFPS == 0) {
                g_newDataReady.store(true, std::memory_order_release);
            }
            return;
        }

        if (g_overlayHwnd) {
            RECT currentRect;
            GetClientRect(gdHwnd, &currentRect);
            POINT screenTopLeft = {0, 0};
            ClientToScreen(gdHwnd, &screenTopLeft);
            
            RECT screenRect = {screenTopLeft.x, screenTopLeft.y, 
                              screenTopLeft.x + currentRect.right, 
                              screenTopLeft.y + currentRect.bottom};
            
            if (memcmp(&g_lastWindowRect, &screenRect, sizeof(RECT)) != 0) {
                SetWindowPos(g_overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                g_lastWindowRect = screenRect;
            }
        }

        RECT clientRect;
        GetClientRect(gdHwnd, &clientRect); 
        
        POINT screenTopLeft = {0, 0};
        ClientToScreen(gdHwnd, &screenTopLeft); 

        auto director = cocos2d::CCDirector::sharedDirector();
        CCSize winSize = director->getWinSize();
        
        float scaleX = (float)(clientRect.right - clientRect.left) / winSize.width;
        float scaleY = (float)(clientRect.bottom - clientRect.top) / winSize.height;

        auto toImU32 = [](cocos2d::ccColor3B color) -> ImU32 {
            return IM_COL32(color.r, color.g, color.b, 255);
        };

        ImU32 colSolid = toImU32(mod->getSettingValue<cocos2d::ccColor3B>("solid-color"));
        ImU32 colDanger = toImU32(mod->getSettingValue<cocos2d::ccColor3B>("danger-color"));
        ImU32 colOther = IM_COL32(0, 255, 0, 255);
        ImU32 colTriggers = IM_COL32(255, 0, 230, 255);
        ImU32 colPlayer = IM_COL32(255, 255, 0, 255);
        ImU32 colPlayerInner = IM_COL32(0, 255, 50, 255);
        ImU32 colPlayerRot = IM_COL32(255, 128, 0, 255);

        auto* currentFrameShapes = g_bufferPool.getWriteBuffer();
        currentFrameShapes->clear(); 
        currentFrameShapes->reserve(1500); 

        bool renderTriggers = false;

        auto queueRect = [&](cocos2d::CCNode* parentNode, cocos2d::CCRect const& rect, ImU32 color) {
            cocos2d::CCPoint worldTopLeft = parentNode->convertToWorldSpace({rect.getMinX(), rect.getMaxY()});
            cocos2d::CCPoint worldBotRight = parentNode->convertToWorldSpace({rect.getMaxX(), rect.getMinY()});

            auto uiTopLeft = director->convertToUI(worldTopLeft);
            auto uiBotRight = director->convertToUI(worldBotRight);

            RECT winRect;
            winRect.left = screenTopLeft.x + static_cast<int>(uiTopLeft.x * scaleX);
            winRect.top = screenTopLeft.y + static_cast<int>(uiTopLeft.y * scaleY);
            winRect.right = screenTopLeft.x + static_cast<int>(uiBotRight.x * scaleX);
            winRect.bottom = screenTopLeft.y + static_cast<int>(uiBotRight.y * scaleY);

            currentFrameShapes->push_back({ShapeType::Rect, winRect, {}, {0, 0}, 0.0f, color});
        };

        auto queuePolygon = [&](cocos2d::CCNode* parentNode, std::vector<cocos2d::CCPoint> const& points, ImU32 color) {
            std::vector<Vec2i> winPoints;
            winPoints.reserve(points.size());
            for (auto const& pt : points) {
                cocos2d::CCPoint worldPt = parentNode->convertToWorldSpace(pt);
                auto uiPt = director->convertToUI(worldPt);
                winPoints.push_back({
                    static_cast<int>(screenTopLeft.x + (uiPt.x * scaleX)),
                    static_cast<int>(screenTopLeft.y + (uiPt.y * scaleY))
                });
            }
            currentFrameShapes->push_back({ShapeType::Polygon, {}, winPoints, {0, 0}, 0.0f, color});
        };

        auto queueCircle = [&](cocos2d::CCNode* parentNode, cocos2d::CCPoint const& centerPos, float radius, ImU32 color) {
            cocos2d::CCPoint worldCenter = parentNode->convertToWorldSpace(centerPos);
            auto uiCenter = director->convertToUI(worldCenter);

            Vec2i winCenter = {
                static_cast<int>(screenTopLeft.x + (uiCenter.x * scaleX)),
                static_cast<int>(screenTopLeft.y + (uiCenter.y * scaleY))
            };

            float winRadius = radius * scaleX; 

            currentFrameShapes->push_back({ShapeType::Circle, {}, {}, winCenter, winRadius, color});
        };

        auto const visitObject = [&](GameObject* obj) {
            if (obj->m_objectType == GameObjectType::Decoration || !obj->m_isActivated || obj->m_isGroupDisabled) return;
            
            cocos2d::CCNode* parent = obj->getParent();
            if (!parent) {
                parent = this->m_objectLayer;
            }
            
            if (!parent) return; 

            switch (obj->m_objectType) {
                case GameObjectType::Slope: {
                    auto rect = obj->getObjectRect();
                    cocos2d::CCPoint p1 = {rect.getMinX(), rect.getMinY()};
                    cocos2d::CCPoint p2 = {rect.getMinX(), rect.getMaxY()};
                    cocos2d::CCPoint p3 = {rect.getMaxX(), rect.getMinY()};
                    cocos2d::CCPoint topRight = {rect.getMaxX(), rect.getMaxY()};

                    switch (obj->m_slopeDirection) {
                        case 0: case 7: p2 = topRight; break;
                        case 1: case 5: p1 = topRight; break;
                        case 3: case 6: p3 = topRight; break;
                        default: break;
                    }
                    queuePolygon(parent, { p1, p2, p3 }, colSolid);
                    break;
                }
                case GameObjectType::Solid: {
                    queueRect(parent, obj->getObjectRect(), colSolid);
                    break;
                }
                case GameObjectType::AnimatedHazard:
                case GameObjectType::Hazard: {
                    if (!obj->m_isActivated || obj == m_anticheatSpike) break;
                    
                    float radius = std::max(obj->getScaleX(), obj->getScaleY()) * obj->m_objectRadius;
                    
                    if (radius > 0.0f) {
                        queueCircle(parent, obj->getPosition(), radius, colDanger);
                    } 
                    else if (auto ob = obj->m_orientedBox) {
                        std::vector<cocos2d::CCPoint> pts(std::begin(ob->m_corners), std::end(ob->m_corners));
                        queuePolygon(parent, pts, colDanger);
                    } 
                    else {
                        queueRect(parent, obj->getObjectRect(), colDanger);
                    }
                    break;
                }
                default: {
                    auto isSpeedPortal = obj->m_objectID == 200 || obj->m_objectID == 201 ||
                        obj->m_objectID == 202 || obj->m_objectID == 203 || obj->m_objectID == 1334;

                    if (obj->m_objectType == GameObjectType::Modifier && !isSpeedPortal) {
                        if (!renderTriggers || !static_cast<EffectGameObject*>(obj)->m_isTouchTriggered) return;
                        queueRect(parent, obj->getObjectRect(), colTriggers);
                        return;
                    }
                    if (obj == m_player1 || obj == m_player2) return;
                    
                    if (auto ob = obj->m_orientedBox) {
                        std::vector<cocos2d::CCPoint> pts(std::begin(ob->m_corners), std::end(ob->m_corners));
                        queuePolygon(parent, pts, colOther);
                    } else {
                        queueRect(parent, obj->getObjectRect(), colOther);
                    }
                    break;
                }
            }
        };

        forEachObject(this, visitObject);

        cocos2d::CCNode* cameraLayer = this->m_objectLayer; 

        auto const drawPlayer = [&](PlayerObject* player, std::deque<PlayerTrailState>& trail) {
            if (!player) return;
            
            PlayerTrailState state;
            state.rect1 = player->getObjectRect();
            state.rect2 = player->getObjectRect(0.3f, 0.3f);
            if (auto ob = player->m_orientedBox) {
                state.rotPts.assign(std::begin(ob->m_corners), std::end(ob->m_corners));
                state.hasRot = true;
            } else {
                state.hasRot = false;
            }

            if (g_currentSettings.hitboxTrail && g_currentSettings.trailLength > 0) {
                bool positionChanged = true;
                if (!trail.empty()) {
                    if (trail.front().rect1.getMinX() == state.rect1.getMinX() && trail.front().rect1.getMinY() == state.rect1.getMinY()) {
                        positionChanged = false;
                    }
                }
                
                if (positionChanged) {
                    trail.push_front(state);
                    while (trail.size() > static_cast<size_t>(g_currentSettings.trailLength)) {
                        trail.pop_back();
                    }
                }

                for (size_t i = 1; i < trail.size(); ++i) {
                    if (trail[ i ].hasRot) queuePolygon(cameraLayer, trail[ i ].rotPts, colPlayerRot);
                    queueRect(cameraLayer, trail[ i ].rect1, colPlayer);
                    queueRect(cameraLayer, trail[ i ].rect2, colPlayerInner);
                }
            } else {
                trail.clear();
            }

            if (state.hasRot) queuePolygon(cameraLayer, state.rotPts, colPlayerRot);
            queueRect(cameraLayer, state.rect1, colPlayer);
            queueRect(cameraLayer, state.rect2, colPlayerInner);
        };

        drawPlayer(m_player1, g_p1Trail);
        if (m_gameState.m_isDualMode) drawPlayer(m_player2, g_p2Trail);

        g_bufferPool.swapWriteBuffer();
        
        g_newDataReady.store(true, std::memory_order_release);
#endif
    }
};

class $modify(ShowHitboxesPLHook, PlayLayer) {
    void updateVisibility(float dt) override {
        PlayLayer::updateVisibility(dt);
        s_isDead = m_player1->m_isDead;
        static_cast<ShowHitboxesGJBGLHook*>(static_cast<GJBaseGameLayer*>(this))->visitHitboxes();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        s_isDead = false;
        s_collisionObject = nullptr;
        g_p1Trail.clear();
        g_p2Trail.clear();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        if (m_anticheatSpike == object) return;
        if (object != nullptr) s_collisionObject = object;
    }
};

class $modify(ShowHitboxesPOHook, PlayerObject) {
    void playerDestroyed(bool p0) {
        if (auto* pl = PlayLayer::get())
            s_isDead = this == pl->m_player1 || this == pl->m_player2;
        PlayerObject::playerDestroyed(p0);
    }

    void collidedWithObject(float p0, GameObject* p1, cocos2d::CCRect p2, bool p3) {
        PlayerObject::collidedWithObject(p0, p1, p2, p3);
        if (auto* pl = PlayLayer::get())
            if (this == pl->m_player1 || this == pl->m_player2)
                s_collisionObject = p1;
    }
};

class $modify(ShowHitboxesLELHook, LevelEditorLayer) {  
    void updateVisibility(float dt) override {
        LevelEditorLayer::updateVisibility(dt);
        static_cast<ShowHitboxesGJBGLHook*>(static_cast<GJBaseGameLayer*>(this))->visitHitboxes();
    }
};

$execute {
}