#include "game_view.hpp"

#include <algorithm>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <cstring>

namespace NeXo2::QtUi {

namespace {
// Barra de arriba con fondo degradado (de negro a transparente)
class TopBar : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        QLinearGradient g(0, 0, 0, height());
        g.setColorAt(0, QColor(0, 0, 0, 200));
        g.setColorAt(1, QColor(0, 0, 0, 0));
        p.fillRect(rect(), g);
    }
};
} // namespace

GameView::GameView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(::Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(::Qt::WA_OpaquePaintEvent);

    m_barWidget = new TopBar(this);
    m_barWidget->setMouseTracking(true);
    auto* bl = new QHBoxLayout(m_barWidget);
    bl->setContentsMargins(22, 14, 22, 30);
    bl->setSpacing(10);
    m_titleLabel = new QLabel(m_barWidget);
    m_titleLabel->setStyleSheet("color: white; font-size: 16px; font-weight: 600; background: transparent;");
    bl->addWidget(m_titleLabel);
    bl->addStretch();
    m_hudBtn = new IconButton(Icon::Chip, "Rendimiento (F3)", m_barWidget);
    m_pause = new IconButton(Icon::Pause, "Pausa (F5)", m_barWidget);
    m_stop = new IconButton(Icon::Stop, "Detener (Esc dos veces)", m_barWidget);
    m_full = new IconButton(Icon::Fullscreen, "Pantalla completa (F11)", m_barWidget);
    for (auto* b : {m_hudBtn, m_pause, m_stop, m_full}) { b->setFocusPolicy(::Qt::NoFocus); bl->addWidget(b); }
    m_hudBtn->SetChecked(true);
    connect(m_pause, &QAbstractButton::clicked, this, &GameView::PauseToggled);
    connect(m_stop, &QAbstractButton::clicked, this, &GameView::StopRequested);
    connect(m_full, &QAbstractButton::clicked, this, &GameView::FullscreenToggled);
    connect(m_hudBtn, &QAbstractButton::clicked, this, &GameView::HudToggled);

    m_lastMove.start();
    m_anim.setInterval(16);
    connect(&m_anim, &QTimer::timeout, this, [this] {
        // La barra se esconde a los 2,5 s sin mover el raton (si no esta en pausa)
        if (!m_paused && m_lastMove.elapsed() > 2500) m_barTarget = 0;
        const qreal before = m_bar;
        m_bar += (m_barTarget - m_bar) * 0.18;
        if (std::abs(m_bar - m_barTarget) < 0.01) m_bar = m_barTarget;
        if (m_bar != before) {
            m_barWidget->setVisible(m_bar > 0.02);
            PlaceBar();
            update();
        }
        if (m_bar < 0.02 && !m_paused) setCursor(::Qt::BlankCursor);
        if (m_frame.isNull()) { m_spin += 0.12; update(); }
    });
    m_anim.start();
}

void GameView::PlaceBar() {
    const int h = 84;
    m_barWidget->setGeometry(0, int(-h * (1 - m_bar)), width(), h);
}

void GameView::resizeEvent(QResizeEvent*) { PlaceBar(); }

void GameView::SetFrame(const std::vector<u32>& rgba, u32 width, u32 height) {
    if (m_frame.width() != int(width) || m_frame.height() != int(height))
        m_frame = QImage(int(width), int(height), QImage::Format_RGBX8888);
    std::memcpy(m_frame.bits(), rgba.data(), size_t(width) * height * 4);
    m_message.clear();
    update();
}

void GameView::SetTitle(const QString& t) {
    m_title = t;
    m_titleLabel->setText(t);
    update();
}

void GameView::ClearFrame() { m_frame = QImage(); update(); }

void GameView::SetPaused(bool p) {
    m_paused = p;
    m_pause->SetIcon(p ? Icon::Play : Icon::Pause);
    m_pause->setToolTip(p ? "Continuar (F5)" : "Pausa (F5)");
    if (p) { m_barTarget = 1; unsetCursor(); }
    update();
}

void GameView::SetStats(double fps, double mips, const QString& renderer, const QString& pad) {
    m_fps = fps; m_mips = mips; m_renderer = renderer; m_pad = pad;
    m_hudBtn->setChecked(m_hud);
    if (m_hud) update();
}

void GameView::mouseMoveEvent(QMouseEvent*) {
    m_lastMove.restart();
    m_barTarget = 1;
    unsetCursor();
}

void GameView::mouseDoubleClickEvent(QMouseEvent*) { emit FullscreenToggled(); }

void GameView::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    switch (e->key()) {
        case ::Qt::Key_F5: emit PauseToggled(); return;
        case ::Qt::Key_F11: emit FullscreenToggled(); return;
        case ::Qt::Key_F3: emit HudToggled(); return;
        default: break;
    }
    m_keys.insert(e->key());
}

void GameView::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    m_keys.remove(e->key());
}

// Teclado -> mando (igual que NeXo2.exe):
//   Flechas = cruceta  X = A  Z = B  S = X  A = Y  Q = L  W = R  1 = ZL  2 = ZR
//   Intro = +  Retroceso = -  T/F/G/H = stick izquierdo  I/J/K/L = stick derecho
HLE::PadInput GameView::KeyboardPad() const {
    using namespace HLE;
    PadInput pad;
    const struct { int key; u64 button; } map[] = {
        {::Qt::Key_Up, NpadButton::Up}, {::Qt::Key_Down, NpadButton::Down}, {::Qt::Key_Left, NpadButton::Left},
        {::Qt::Key_Right, NpadButton::Right}, {::Qt::Key_X, NpadButton::A}, {::Qt::Key_Z, NpadButton::B},
        {::Qt::Key_S, NpadButton::X}, {::Qt::Key_A, NpadButton::Y}, {::Qt::Key_Q, NpadButton::L},
        {::Qt::Key_W, NpadButton::R}, {::Qt::Key_1, NpadButton::ZL}, {::Qt::Key_2, NpadButton::ZR},
        {::Qt::Key_Return, NpadButton::Plus}, {::Qt::Key_Enter, NpadButton::Plus}, {::Qt::Key_Backspace, NpadButton::Minus},
    };
    for (const auto& m : map) if (m_keys.contains(m.key)) pad.buttons |= m.button;
    auto stick = [this](int neg, int pos) {
        return (m_keys.contains(pos) ? STICK_MAX : 0) - (m_keys.contains(neg) ? STICK_MAX : 0);
    };
    pad.lx = stick(::Qt::Key_F, ::Qt::Key_H);
    pad.ly = stick(::Qt::Key_G, ::Qt::Key_T);
    pad.rx = stick(::Qt::Key_J, ::Qt::Key_L);
    pad.ry = stick(::Qt::Key_K, ::Qt::Key_I);
    return pad;
}

void GameView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(4, 5, 8));
    p.setRenderHint(QPainter::Antialiasing);

    if (!m_frame.isNull()) {
        // Centrada y con su proporcion
        const QSizeF fs = QSizeF(m_frame.size()).scaled(size(), ::Qt::KeepAspectRatio);
        const QRectF dst((width() - fs.width()) / 2, (height() - fs.height()) / 2, fs.width(), fs.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
        p.drawImage(dst, m_frame);
    } else {
        // Cargando: un arco que gira con el degradado de la marca
        const QPointF c = rect().center();
        QPen pen(QBrush(AccentGradient(c - QPointF(24, 24), c + QPointF(24, 24))), 4);
        pen.setCapStyle(::Qt::RoundCap);
        p.setPen(pen);
        p.drawArc(QRectF(c.x() - 22, c.y() - 22, 44, 44), int(-m_spin * 57.3 * 16), 100 * 16);
        p.setPen(Color::TextDim);
        QFont f = font();
        f.setPointSizeF(11);
        p.setFont(f);
        p.drawText(QRectF(0, c.y() + 36, width(), 30), ::Qt::AlignHCenter,
                   m_message.isEmpty() ? QString("Arrancando ") + m_title + "…" : m_message);
    }

    // Indicador de rendimiento (abajo a la izquierda)
    if (m_hud && !m_frame.isNull()) {
        QFont f = font();
        f.setPointSizeF(9);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        const QString fps = QString::number(m_fps, 'f', 0) + " FPS";
        const QString rest = QString("%1 MIPS  ·  %2%3").arg(m_mips, 0, 'f', 0).arg(m_renderer)
                                 .arg(m_pad.isEmpty() ? "" : "  ·  " + m_pad);
        const QFontMetricsF fm(f);
        const qreal w = fm.horizontalAdvance(fps) + fm.horizontalAdvance(rest) + 54;
        const QRectF r(16, height() - 46, w, 30);
        p.setPen(::Qt::NoPen);
        p.setBrush(QColor(10, 12, 18, 200));
        p.drawRoundedRect(r, 15, 15);
        const QColor dot = m_fps >= 55 ? Color::Good : m_fps >= 28 ? Color::Warn : Color::Bad;
        p.setBrush(dot);
        p.drawEllipse(QPointF(r.left() + 16, r.center().y()), 4, 4);
        p.setPen(::Qt::white);
        p.drawText(QRectF(r.left() + 28, r.top(), fm.horizontalAdvance(fps) + 4, r.height()), ::Qt::AlignVCenter, fps);
        p.setPen(Color::TextDim);
        p.drawText(QRectF(r.left() + 36 + fm.horizontalAdvance(fps), r.top(), fm.horizontalAdvance(rest) + 6, r.height()),
                   ::Qt::AlignVCenter, rest);
    }

    // En pausa: oscurecer y un rotulo
    if (m_paused) {
        p.fillRect(rect(), QColor(5, 6, 10, 150));
        const QPointF c = rect().center();
        p.setPen(::Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 24));
        p.drawEllipse(c, 46, 46);
        DrawIcon(p, Icon::Pause, QRectF(c.x() - 20, c.y() - 20, 40, 40), ::Qt::white);
        QFont f = font();
        f.setPointSizeF(13);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(::Qt::white);
        p.drawText(QRectF(0, c.y() + 56, width(), 30), ::Qt::AlignHCenter, "En pausa");
        f.setPointSizeF(10);
        f.setWeight(QFont::Normal);
        p.setFont(f);
        p.setPen(Color::TextDim);
        p.drawText(QRectF(0, c.y() + 84, width(), 24), ::Qt::AlignHCenter, "Pulsa F5 para continuar");
    }
}

} // namespace NeXo2::QtUi
