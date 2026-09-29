#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <iostream>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

int main() {
    CoInitialize(NULL);
    
    ID2D1Factory1* d2dFactory = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory);
    
    IDWriteFactory3* dwriteFactory = nullptr;
    IDWriteFactory* dwriteBase = nullptr;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwriteBase);
    dwriteBase->QueryInterface(__uuidof(IDWriteFactory3), (void**)&dwriteFactory);
    
    std::cout << "Factories created" << std::endl;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT
    );
    
    ID2D1DCRenderTarget* dcRenderTarget = nullptr;
    HRESULT hr = d2dFactory->CreateDCRenderTarget(&props, &dcRenderTarget);
    std::cout << "CreateDCRenderTarget hr = 0x" << std::hex << hr << std::endl;

    IDWriteRenderingParams3* textRenderingParams = nullptr;
    hr = dwriteFactory->CreateCustomRenderingParams(
        1.8f,
        1.0f,
        1.0f,
        0.0f,
        DWRITE_PIXEL_GEOMETRY_FLAT,
        DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC,
        DWRITE_GRID_FIT_MODE_DISABLED,
        &textRenderingParams
    );
    std::cout << "CreateCustomRenderingParams hr = 0x" << std::hex << hr << std::endl;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 800;
    bmi.bmiHeader.biHeight = -600;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(hdcMem, hBitmap);
    
    RECT rc = { 0, 0, 800, 600 };
    hr = dcRenderTarget->BindDC(hdcMem, &rc);
    std::cout << "BindDC hr = 0x" << std::hex << hr << std::endl;

    dcRenderTarget->BeginDraw();
    dcRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dcRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dcRenderTarget->SetTextRenderingParams(textRenderingParams);
    dcRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    IDWriteTextFormat* tf = nullptr;
    dwriteFactory->CreateTextFormat(L"Arial", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-US", &tf);
    
    IDWriteTextLayout* layout = nullptr;
    dwriteFactory->CreateTextLayout(L"Testing text layout", 19, tf, 400.0f, 100.0f, &layout);

    ID2D1SolidColorBrush* brush = nullptr;
    dcRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush);

    std::cout << "About to DrawTextLayout with ENABLE_COLOR_FONT..." << std::endl;
    dcRenderTarget->DrawTextLayout(
        D2D1::Point2F(10.0f, 10.0f),
        layout,
        brush,
        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
    );
    std::cout << "DrawTextLayout returned" << std::endl;

    std::cout << "About to EndDraw..." << std::endl;
    hr = dcRenderTarget->EndDraw();
    std::cout << "EndDraw hr = 0x" << std::hex << hr << std::endl;

    return 0;
}
