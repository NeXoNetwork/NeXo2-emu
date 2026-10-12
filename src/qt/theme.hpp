#pragma once
// Aspecto de NeXo2-Qt: colores, hoja de estilo (QSS) e iconos dibujados con lineas (sin
// archivos de imagen ni fuentes de iconos: se ven nitidos a cualquier escala).
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QRectF>
#include <QString>

class QApplication;
class QWidget;

namespace NeXo2::QtUi {

namespace Color {
inline const QColor Bg("#0B0E14");          // fondo de la ventana
inline const QColor Sidebar("#0F131B");
inline const QColor Surface("#151A24");     // tarjetas
inline const QColor SurfaceHi("#1C2230");   // tarjeta con el raton encima
inline const QColor Border("#252C3B");
inline const QColor Text("#E9EDF5");
inline const QColor TextDim("#8C95A8");
inline const QColor TextFaint("#5B6476");
inline const QColor Accent("#7C5CFF");      // violeta
inline const QColor Accent2("#00C8FF");     // cian (el degradado va de uno a otro)
inline const QColor Good("#3DDC97");
inline const QColor Warn("#FFB547");
inline const QColor Bad("#FF5C7A");
}

// Degradado de la marca entre dos puntos
QLinearGradient AccentGradient(QPointF a, QPointF b, int alpha = 255);

// Tema oscuro para toda la aplicacion (paleta Fusion + hoja de estilo)
void ApplyTheme(QApplication& app);
// Barra de titulo oscura en Windows 10/11 (en otros sistemas no hace nada)
void DarkTitleBar(QWidget* window);

enum class Icon {
    Library, Play, Pause, Stop, Settings, Info, FolderPlus, File, Search, Fullscreen, ExitFullscreen,
    Gamepad, Chip, Gpu, Volume, Refresh, Folder, Grid, List, Trash, Back, Keyboard, Monitor, Sparkle
};
// Dibuja un icono de lineas dentro de 'r'
void DrawIcon(QPainter& p, Icon icon, const QRectF& r, const QColor& color, qreal stroke = 1.8);
QIcon MakeIcon(Icon icon, const QColor& color, int size = 20);

} // namespace NeXo2::QtUi
