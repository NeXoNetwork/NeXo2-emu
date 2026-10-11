#include "theme.hpp"
#include "imgui.h"
#include <filesystem>

namespace NeXo2::Frontend {

void ApplyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(8, 6);
    s.ScrollbarSize = 12;
    s.GrabMinSize = 10;
    s.WindowRounding = 8;
    s.ChildRounding = 6;
    s.FrameRounding = 6;
    s.PopupRounding = 6;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 6;
    s.TabRounding = 6;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;

    const ImVec4 bg      = ImVec4(0.067f, 0.071f, 0.082f, 1.00f);   // #111215
    const ImVec4 bg2     = ImVec4(0.098f, 0.102f, 0.118f, 1.00f);   // #191a1e
    const ImVec4 bg3     = ImVec4(0.137f, 0.141f, 0.161f, 1.00f);   // #232429
    const ImVec4 bg4     = ImVec4(0.180f, 0.184f, 0.212f, 1.00f);
    const ImVec4 text    = ImVec4(0.925f, 0.918f, 0.894f, 1.00f);   // #ecead4-ish
    const ImVec4 muted   = ImVec4(0.560f, 0.553f, 0.535f, 1.00f);
    const ImVec4 accent  = ImVec4(0.898f, 0.212f, 0.122f, 1.00f);   // #e5361f
    const ImVec4 accentH = ImVec4(0.949f, 0.290f, 0.192f, 1.00f);
    const ImVec4 border  = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    auto a = [](ImVec4 c, float alpha) { c.w = alpha; return c; };

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = bg2;
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = bg3;
    c[ImGuiCol_FrameBgHovered] = bg4;
    c[ImGuiCol_FrameBgActive] = bg4;
    c[ImGuiCol_TitleBg] = bg2;
    c[ImGuiCol_TitleBgActive] = bg3;
    c[ImGuiCol_TitleBgCollapsed] = bg2;
    c[ImGuiCol_MenuBarBg] = bg2;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = bg4;
    c[ImGuiCol_ScrollbarGrabHovered] = a(muted, 0.6f);
    c[ImGuiCol_ScrollbarGrabActive] = muted;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentH;
    c[ImGuiCol_Button] = bg3;
    c[ImGuiCol_ButtonHovered] = bg4;
    c[ImGuiCol_ButtonActive] = a(accent, 0.8f);
    c[ImGuiCol_Header] = a(accent, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_HeaderActive] = a(accent, 0.40f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = a(accent, 0.6f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = a(accent, 0.6f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = bg2;
    c[ImGuiCol_TabHovered] = bg4;
    c[ImGuiCol_TabSelected] = bg3;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg2;
    c[ImGuiCol_TabDimmedSelected] = bg3;
    c[ImGuiCol_TableHeaderBg] = bg2;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = border;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextSelectedBg] = a(accent, 0.35f);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}

Fonts LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    Fonts f;
    const char* regular[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    const char* bold[] = {
        "C:/Windows/Fonts/seguisb.ttf",
        "/usr/share/fonts/opentype/inter/Inter-SemiBold.otf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    };
    std::error_code ec;
    for (const char* p : regular)
        if (std::filesystem::exists(p, ec)) { f.regular = io.Fonts->AddFontFromFileTTF(p, 17.0f); if (f.regular) break; }
    for (const char* p : bold)
        if (std::filesystem::exists(p, ec)) { f.bold = io.Fonts->AddFontFromFileTTF(p, 17.0f); if (f.bold) break; }
    if (!f.regular) f.regular = io.Fonts->AddFontDefault();
    if (!f.bold) f.bold = f.regular;
    io.FontDefault = f.regular;
    ImGui::GetStyle().FontSizeBase = 17.0f;
    return f;
}

} // namespace NeXo2::Frontend
