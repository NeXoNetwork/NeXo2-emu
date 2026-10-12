#include "theme.hpp"

#include <algorithm>
#include <QApplication>
#include <QDir>
#include <QFont>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>
#include <QWidget>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

namespace NeXo2::QtUi {

QLinearGradient AccentGradient(QPointF a, QPointF b, int alpha) {
    QLinearGradient g(a, b);
    QColor c1 = Color::Accent, c2 = Color::Accent2;
    c1.setAlpha(alpha);
    c2.setAlpha(alpha);
    g.setColorAt(0, c1);
    g.setColorAt(1, c2);
    return g;
}

void ApplyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QFont font = app.font();
#ifdef _WIN32
    font.setFamilies({"Segoe UI Variable Text", "Segoe UI"});
#else
    font.setFamilies({"Inter", "Noto Sans", "DejaVu Sans"});
#endif
    font.setPointSizeF(10);
    font.setHintingPreference(QFont::PreferNoHinting);
    app.setFont(font);

    QPalette p;
    p.setColor(QPalette::Window, Color::Bg);
    p.setColor(QPalette::WindowText, Color::Text);
    p.setColor(QPalette::Base, Color::Surface);
    p.setColor(QPalette::AlternateBase, Color::SurfaceHi);
    p.setColor(QPalette::Text, Color::Text);
    p.setColor(QPalette::Button, Color::Surface);
    p.setColor(QPalette::ButtonText, Color::Text);
    p.setColor(QPalette::Highlight, Color::Accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, Color::SurfaceHi);
    p.setColor(QPalette::ToolTipText, Color::Text);
    p.setColor(QPalette::PlaceholderText, Color::TextFaint);
    p.setColor(QPalette::Disabled, QPalette::Text, Color::TextFaint);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, Color::TextFaint);
    app.setPalette(p);

    // Flecha de los desplegables (la hoja de estilo necesita una imagen): se dibuja aqui
    const QString chevron = QDir::temp().filePath("nexo2-chevron.png");
    {
        QPixmap pm(24, 24);
        pm.fill(::Qt::transparent);
        QPainter cp(&pm);
        cp.setRenderHint(QPainter::Antialiasing);
        QPen pen(Color::TextDim, 2.2);
        pen.setCapStyle(::Qt::RoundCap);
        pen.setJoinStyle(::Qt::RoundJoin);
        cp.setPen(pen);
        cp.drawPolyline(QPolygonF({QPointF(7, 10), QPointF(12, 15), QPointF(17, 10)}));
        cp.end();
        pm.save(chevron);
    }
    // Hoja de estilo: bordes redondeados, superficies suaves y el violeta de la marca
    app.setStyleSheet(QStringLiteral(R"(
        QWidget { color: #E9EDF5; }
        QMainWindow, #page { background: #0B0E14; }
        QToolTip { background: #1C2230; color: #E9EDF5; border: 1px solid #2C3446; border-radius: 6px; padding: 6px 8px; }

        #sidebar { background: #0F131B; border-right: 1px solid #1A202C; }
        #pageTitle { font-size: 26px; font-weight: 700; letter-spacing: -0.5px; }
        #pageSubtitle { color: #8C95A8; font-size: 13px; }
        #sectionTitle { font-size: 15px; font-weight: 650; }
        #dim { color: #8C95A8; }
        #faint { color: #5B6476; }

        QLineEdit {
            background: #151A24; border: 1px solid #252C3B; border-radius: 10px;
            padding: 9px 12px 9px 36px; selection-background-color: #7C5CFF; font-size: 13px;
        }
        QLineEdit:focus { border: 1px solid #7C5CFF; background: #171C28; }

        QPushButton {
            background: #1A2030; border: 1px solid #262E40; border-radius: 10px;
            padding: 9px 16px; font-weight: 600;
        }
        QPushButton:hover { background: #222A3C; border-color: #34405A; }
        QPushButton:pressed { background: #161B27; }
        QPushButton:disabled { color: #5B6476; background: #141822; }
        QPushButton#primary {
            border: none; color: white;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #7C5CFF, stop:1 #00C8FF);
        }
        QPushButton#primary:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8E72FF, stop:1 #33D4FF);
        }
        QPushButton#primary:pressed {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6B4BEE, stop:1 #00B2E6);
        }
        QPushButton#ghost { background: transparent; border: none; color: #8C95A8; padding: 6px 10px; }
        QPushButton#ghost:hover { color: #E9EDF5; background: #1A2030; }
        QPushButton#danger { color: #FF5C7A; }

        QComboBox {
            background: #1A2030; border: 1px solid #262E40; border-radius: 9px; padding: 7px 12px; min-width: 200px;
        }
        QComboBox:hover { border-color: #34405A; }
        QComboBox::drop-down { border: none; width: 30px; }
        QComboBox::down-arrow { image: url(CHEVRON); width: 14px; height: 14px; margin-right: 8px; }
        QComboBox QAbstractItemView {
            background: #1A2030; border: 1px solid #2C3446; border-radius: 8px; padding: 4px;
            selection-background-color: #2A2350; outline: none;
        }

        QSlider::groove:horizontal { height: 6px; background: #252C3B; border-radius: 3px; }
        QSlider::sub-page:horizontal {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #7C5CFF, stop:1 #00C8FF); border-radius: 3px;
        }
        QSlider::handle:horizontal { background: white; width: 16px; height: 16px; margin: -5px 0; border-radius: 8px; }

        QListWidget#folders { background: #11151E; border: 1px solid #222939; border-radius: 10px; padding: 4px; }
        QListWidget#folders::item { padding: 8px 10px; border-radius: 6px; }
        QListWidget#folders::item:selected { background: #2A2350; color: white; }

        QScrollArea { border: none; background: transparent; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 2px; }
        QScrollBar::handle:vertical { background: #2A3142; border-radius: 3px; min-height: 40px; }
        QScrollBar::handle:vertical:hover { background: #3A4358; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QScrollBar:horizontal { height: 0; }

        QListView#library { background: transparent; border: none; outline: none; }

        QMenu { background: #171C27; border: 1px solid #2A3244; border-radius: 10px; padding: 6px; }
        QMenu::item { padding: 8px 18px 8px 12px; border-radius: 6px; }
        QMenu::item:selected { background: #2A2350; }
        QMenu::separator { height: 1px; background: #2A3244; margin: 5px 8px; }

        QMessageBox { background: #121620; }
        QMessageBox QLabel { color: #E9EDF5; }

        #card { background: #131823; border: 1px solid #1E2533; border-radius: 16px; }
        #chip { background: #151A24; border: 1px solid #222939; border-radius: 9px; padding: 3px 9px; color: #8C95A8; font-size: 12px; }
    )").replace("CHEVRON", QDir::fromNativeSeparators(chevron)));
}

void DarkTitleBar(QWidget* window) {
#ifdef _WIN32
    if (!window) return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(0x0B, 0x0E, 0x14);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof(caption));
#else
    (void)window;
#endif
}

// --- Iconos -------------------------------------------------------------------
// Coordenadas en una rejilla de 24x24 (como los iconos de lineas habituales)
void DrawIcon(QPainter& p, Icon icon, const QRectF& r, const QColor& color, qreal stroke) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(r.topLeft());
    const qreal s = std::min(r.width(), r.height()) / 24.0;
    p.scale(s, s);
    QPen pen(color, stroke);
    pen.setCapStyle(::Qt::RoundCap);
    pen.setJoinStyle(::Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(::Qt::NoBrush);
    auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) { p.drawLine(QPointF(x1, y1), QPointF(x2, y2)); };
    auto rrect = [&](qreal x, qreal y, qreal w, qreal h, qreal rad) { p.drawRoundedRect(QRectF(x, y, w, h), rad, rad); };
    QPainterPath path;
    switch (icon) {
        case Icon::Library:
            rrect(3.5, 3.5, 7, 7, 2); rrect(13.5, 3.5, 7, 7, 2); rrect(3.5, 13.5, 7, 7, 2); rrect(13.5, 13.5, 7, 7, 2);
            break;
        case Icon::Grid:
            rrect(4, 4, 6.5, 6.5, 1.5); rrect(13.5, 4, 6.5, 6.5, 1.5); rrect(4, 13.5, 6.5, 6.5, 1.5); rrect(13.5, 13.5, 6.5, 6.5, 1.5);
            break;
        case Icon::List:
            line(9, 6, 20, 6); line(9, 12, 20, 12); line(9, 18, 20, 18);
            p.drawPoint(QPointF(4.5, 6)); p.drawPoint(QPointF(4.5, 12)); p.drawPoint(QPointF(4.5, 18));
            break;
        case Icon::Play:
            path.moveTo(7, 4.5); path.lineTo(19.5, 12); path.lineTo(7, 19.5); path.closeSubpath();
            p.setBrush(color); p.drawPath(path);
            break;
        case Icon::Pause:
            p.setBrush(color); rrect(6, 5, 4, 14, 1.2); rrect(14, 5, 4, 14, 1.2);
            break;
        case Icon::Stop:
            p.setBrush(color); rrect(6, 6, 12, 12, 2.5);
            break;
        case Icon::Settings: {
            p.drawEllipse(QPointF(12, 12), 3, 3);
            for (int i = 0; i < 8; ++i) {
                const qreal a = i * 3.14159265358979 / 4;
                line(12 + std::cos(a) * 6.3, 12 + std::sin(a) * 6.3, 12 + std::cos(a) * 8.8, 12 + std::sin(a) * 8.8);
            }
            p.drawEllipse(QPointF(12, 12), 6.3, 6.3);
            break;
        }
        case Icon::Info:
            p.drawEllipse(QPointF(12, 12), 9, 9); line(12, 11, 12, 16.5); p.drawPoint(QPointF(12, 7.8));
            break;
        case Icon::Folder:
        case Icon::FolderPlus:
            path.moveTo(3.5, 7); path.quadTo(3.5, 5, 5.5, 5); path.lineTo(9.5, 5); path.lineTo(11.5, 7.5);
            path.lineTo(18.5, 7.5); path.quadTo(20.5, 7.5, 20.5, 9.5); path.lineTo(20.5, 17); path.quadTo(20.5, 19, 18.5, 19);
            path.lineTo(5.5, 19); path.quadTo(3.5, 19, 3.5, 17); path.closeSubpath();
            p.drawPath(path);
            if (icon == Icon::FolderPlus) { line(12, 10.5, 12, 16); line(9.25, 13.25, 14.75, 13.25); }
            break;
        case Icon::File:
            path.moveTo(6, 3.5); path.lineTo(14, 3.5); path.lineTo(19, 8.5); path.lineTo(19, 20.5); path.lineTo(6, 20.5); path.closeSubpath();
            p.drawPath(path); line(14, 3.5, 14, 8.5); line(14, 8.5, 19, 8.5);
            break;
        case Icon::Search:
            p.drawEllipse(QPointF(10.5, 10.5), 6, 6); line(15, 15, 20, 20);
            break;
        case Icon::Fullscreen:
            line(4, 9, 4, 4); line(4, 4, 9, 4); line(15, 4, 20, 4); line(20, 4, 20, 9);
            line(20, 15, 20, 20); line(20, 20, 15, 20); line(9, 20, 4, 20); line(4, 20, 4, 15);
            break;
        case Icon::ExitFullscreen:
            line(9, 4, 9, 9); line(9, 9, 4, 9); line(15, 4, 15, 9); line(15, 9, 20, 9);
            line(15, 20, 15, 15); line(15, 15, 20, 15); line(9, 20, 9, 15); line(9, 15, 4, 15);
            break;
        case Icon::Gamepad:
            path.moveTo(7, 7.5); path.lineTo(17, 7.5); path.cubicTo(21, 7.5, 22, 17, 19.5, 18);
            path.cubicTo(17.5, 19, 16, 15.5, 14.5, 15.5); path.lineTo(9.5, 15.5); path.cubicTo(8, 15.5, 6.5, 19, 4.5, 18);
            path.cubicTo(2, 17, 3, 7.5, 7, 7.5);
            p.drawPath(path);
            line(7, 10.5, 7, 13.5); line(5.5, 12, 8.5, 12);
            p.drawPoint(QPointF(16, 11)); p.drawPoint(QPointF(18, 13));
            break;
        case Icon::Chip:
            rrect(6.5, 6.5, 11, 11, 2); rrect(9.5, 9.5, 5, 5, 1);
            for (qreal t : {9.5, 12.0, 14.5}) { line(t, 3.5, t, 6.5); line(t, 17.5, t, 20.5); line(3.5, t, 6.5, t); line(17.5, t, 20.5, t); }
            break;
        case Icon::Gpu:
            rrect(3.5, 6.5, 17, 11, 2); p.drawEllipse(QPointF(14.5, 12), 3, 3); line(7, 10, 7, 14); line(7, 17.5, 7, 20); line(11, 17.5, 11, 20);
            break;
        case Icon::Volume:
            path.moveTo(4, 9.5); path.lineTo(7.5, 9.5); path.lineTo(12, 5.5); path.lineTo(12, 18.5); path.lineTo(7.5, 14.5);
            path.lineTo(4, 14.5); path.closeSubpath(); p.drawPath(path);
            p.drawArc(QRectF(11, 8, 6, 8), -60 * 16, 120 * 16); p.drawArc(QRectF(10, 4.5, 10, 15), -60 * 16, 120 * 16);
            break;
        case Icon::Refresh:
            p.drawArc(QRectF(4.5, 4.5, 15, 15), 30 * 16, 290 * 16); line(19.5, 4, 19.5, 8.5); line(19.5, 8.5, 15, 8.5);
            break;
        case Icon::Trash:
            line(4.5, 7, 19.5, 7); line(9.5, 7, 9.5, 4.5); line(9.5, 4.5, 14.5, 4.5); line(14.5, 4.5, 14.5, 7);
            path.moveTo(6.5, 7); path.lineTo(7.5, 19.5); path.lineTo(16.5, 19.5); path.lineTo(17.5, 7); p.drawPath(path);
            break;
        case Icon::Back:
            line(15, 5, 8, 12); line(8, 12, 15, 19);
            break;
        case Icon::Keyboard:
            rrect(3, 6.5, 18, 11, 2);
            for (qreal x : {6.5, 9.5, 12.5, 15.5}) p.drawPoint(QPointF(x, 10));
            line(8, 14, 16, 14);
            break;
        case Icon::Monitor:
            rrect(3, 4.5, 18, 12, 2); line(12, 16.5, 12, 19.5); line(8, 19.5, 16, 19.5);
            break;
        case Icon::Sparkle:
            path.moveTo(12, 3); path.cubicTo(12.8, 9, 15, 11.2, 21, 12); path.cubicTo(15, 12.8, 12.8, 15, 12, 21);
            path.cubicTo(11.2, 15, 9, 12.8, 3, 12); path.cubicTo(9, 11.2, 11.2, 9, 12, 3);
            p.setBrush(color); p.drawPath(path);
            break;
    }
    p.restore();
}

QIcon MakeIcon(Icon icon, const QColor& color, int size) {
    QIcon result;
    for (int scale : {1, 2}) {
        QPixmap pm(size * scale, size * scale);
        pm.fill(::Qt::transparent);
        pm.setDevicePixelRatio(scale);
        QPainter p(&pm);
        DrawIcon(p, icon, QRectF(0, 0, size, size), color);
        p.end();
        result.addPixmap(pm);
    }
    return result;
}

} // namespace NeXo2::QtUi
