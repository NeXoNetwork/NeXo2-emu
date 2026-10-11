#pragma once
// Aspecto de la interfaz: tema oscuro con el rojo de NeXo como acento y una fuente
// moderna del sistema (Segoe UI en Windows), como los emuladores actuales.
struct ImFont;

namespace NeXo2::Frontend {

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;    // titulos (o la normal si no hay negrita)
};

// Aplica colores, redondeos y espaciados. Llamar despues de ImGui::CreateContext().
void ApplyTheme(float scale);
// Carga las fuentes (si no encuentra ninguna, deja la de ImGui)
Fonts LoadFonts();

// Colores de acento (tambien para dibujar a mano)
constexpr unsigned ACCENT = 0xFF1F36E5;        // rojo NeXo (ABGR para ImGui: IM_COL32)
constexpr unsigned ACCENT_SOFT = 0x661F36E5;

} // namespace NeXo2::Frontend
