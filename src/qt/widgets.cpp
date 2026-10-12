#include "widgets.hpp"

#include <algorithm>
#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSequentialAnimationGroup>

namespace NeXo2::QtUi {

// ---- ToggleSwitch ---------------------------------------------------------------
ToggleSwitch::ToggleSwitch(QWidget* parent) : QAbstractButton(parent), m_anim(this, "pos") {
    setCheckable(true);
    setCursor(::Qt::PointingHandCursor);
    setFixedSize(46, 26);
    m_anim.setDuration(160);
    m_anim.setEasingCurve(QEasingCurve::OutCubic);
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        m_anim.stop();
        m_anim.setStartValue(m_pos);
        m_anim.setEndValue(on ? 1.0 : 0.0);
        m_anim.start();
    });
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    if (!m_anim.state() && m_pos != (isChecked() ? 1.0 : 0.0)) m_pos = isChecked() ? 1.0 : 0.0;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF track(1, 3, width() - 2, height() - 6);
    QColor off = m_hover ? QColor("#323B50") : QColor("#283043");
    p.setPen(::Qt::NoPen);
    p.setBrush(off);
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    if (m_pos > 0) {   // el color de la marca aparece al encender
        p.setOpacity(m_pos);
        p.setBrush(AccentGradient(track.topLeft(), track.topRight()));
        p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
        p.setOpacity(1);
    }
    const qreal d = height() - 4;
    const qreal x = 2 + m_pos * (width() - d - 4);
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawEllipse(QRectF(x, 3, d, d));
    p.setBrush(isEnabled() ? QColor(Qt::white) : QColor("#8C95A8"));
    p.drawEllipse(QRectF(x, 2, d, d));
}

// ---- NavButton ------------------------------------------------------------------
NavButton::NavButton(Icon icon, const QString& text, QWidget* parent)
    : QAbstractButton(parent), m_icon(icon), m_anim(this, "glow") {
    setText(text);
    setCheckable(true);
    setAutoExclusive(true);
    setCursor(::Qt::PointingHandCursor);
    setFixedHeight(44);
    m_anim.setDuration(140);
}

void NavButton::enterEvent(QEnterEvent*) { m_anim.stop(); m_anim.setStartValue(m_glow); m_anim.setEndValue(1.0); m_anim.start(); }
void NavButton::leaveEvent(QEvent*) { m_anim.stop(); m_anim.setStartValue(m_glow); m_anim.setEndValue(0.0); m_anim.start(); }

void NavButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(10, 2, -10, -2);
    p.setPen(::Qt::NoPen);
    if (isChecked()) {
        p.setBrush(AccentGradient(r.topLeft(), r.topRight(), 46));
        p.drawRoundedRect(r, 10, 10);
        p.setBrush(AccentGradient(QPointF(0, r.top()), QPointF(0, r.bottom())));
        p.drawRoundedRect(QRectF(r.left() - 8, r.top() + 10, 3.5, r.height() - 20), 2, 2);
    } else if (m_glow > 0) {
        QColor c("#1A2030");
        c.setAlphaF(m_glow);
        p.setBrush(c);
        p.drawRoundedRect(r, 10, 10);
    }
    const QColor fg = isChecked() ? QColor(Qt::white) : QColor::fromRgbF(
        Color::TextDim.redF() + (Color::Text.redF() - Color::TextDim.redF()) * m_glow,
        Color::TextDim.greenF() + (Color::Text.greenF() - Color::TextDim.greenF()) * m_glow,
        Color::TextDim.blueF() + (Color::Text.blueF() - Color::TextDim.blueF()) * m_glow);
    DrawIcon(p, m_icon, QRectF(r.left() + 12, r.center().y() - 10, 20, 20), fg);
    QFont f = font();
    f.setPointSizeF(10.5);
    f.setWeight(isChecked() ? QFont::DemiBold : QFont::Medium);
    p.setFont(f);
    p.setPen(fg);
    p.drawText(QRectF(r.left() + 44, r.top(), r.width() - 48, r.height()), ::Qt::AlignVCenter | ::Qt::AlignLeft, text());
}

// ---- IconButton -----------------------------------------------------------------
IconButton::IconButton(Icon icon, const QString& tooltip, QWidget* parent, int size)
    : QAbstractButton(parent), m_icon(icon), m_size(size) {
    setToolTip(tooltip);
    setCursor(::Qt::PointingHandCursor);
    setFixedSize(size, size);
}

void IconButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.setPen(::Qt::NoPen);
    if (isCheckable() && isChecked()) p.setBrush(AccentGradient(r.topLeft(), r.bottomRight(), 200));
    else p.setBrush(m_hover ? QColor(255, 255, 255, 36) : QColor(255, 255, 255, 16));
    if (isDown()) p.setBrush(QColor(255, 255, 255, 52));
    p.drawRoundedRect(r, r.height() / 2.6, r.height() / 2.6);
    const qreal is = m_size * 0.5;
    DrawIcon(p, m_icon, QRectF(r.center().x() - is / 2, r.center().y() - is / 2, is, is),
             isEnabled() ? QColor(Qt::white) : Color::TextFaint, 1.9);
}

// ---- Logo -------------------------------------------------------------------------
void Logo::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Marca: un rombo con el degradado y dos lineas que forman una "X"
    const QRectF mark(22, 16, 32, 32);
    QPainterPath diamond;
    diamond.addRoundedRect(mark, 9, 9);
    p.setPen(::Qt::NoPen);
    p.setBrush(AccentGradient(mark.topLeft(), mark.bottomRight()));
    p.drawPath(diamond);
    QPen pen(Qt::white, 3.2);
    pen.setCapStyle(::Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(QPointF(mark.left() + 10, mark.top() + 10), QPointF(mark.right() - 10, mark.bottom() - 10));
    p.drawLine(QPointF(mark.right() - 10, mark.top() + 10), QPointF(mark.left() + 10, mark.bottom() - 10));

    QFont f = font();
    f.setPointSizeF(15);
    f.setWeight(QFont::Bold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, -0.3);
    p.setFont(f);
    p.setPen(Color::Text);
    const QRectF tr(66, 16, width() - 70, 32);
    p.drawText(tr, ::Qt::AlignVCenter | ::Qt::AlignLeft, "NeXo");
    const qreal w = QFontMetricsF(f).horizontalAdvance("NeXo ");
    QPainterPath two;
    two.addText(tr.left() + w, tr.center().y() + QFontMetricsF(f).ascent() / 2 - 2, f, "2");
    p.setPen(::Qt::NoPen);
    p.setBrush(AccentGradient(QPointF(tr.left() + w, 0), QPointF(tr.left() + w + 14, 0)));
    p.drawPath(two);
}

// ---- Toast --------------------------------------------------------------------------
Toast::Toast(QWidget* host, const QString& text, Kind kind) : QWidget(host), m_text(text), m_kind(kind) {
    setAttribute(::Qt::WA_TransparentForMouseEvents);
    QFont f = font();
    f.setPointSizeF(10.5);
    f.setWeight(QFont::Medium);
    setFont(f);
    const int w = std::min(QFontMetrics(f).horizontalAdvance(text) + 76, host->width() - 40);
    resize(w, 50);
    move((host->width() - w) / 2, host->height() - 90);
}

void Toast::Show(QWidget* host, const QString& text, Kind kind, int ms) {
    if (!host) return;
    auto* t = new Toast(host, text, kind);
    t->show();
    t->raise();
    auto* fade_in = new QPropertyAnimation(t, "opacity", t);
    fade_in->setDuration(180);
    fade_in->setStartValue(0.0);
    fade_in->setEndValue(1.0);
    fade_in->start();
    QTimer::singleShot(ms, t, [t] {
        auto* out = new QPropertyAnimation(t, "opacity", t);
        out->setDuration(300);
        out->setStartValue(1.0);
        out->setEndValue(0.0);
        connect(out, &QPropertyAnimation::finished, t, &QObject::deleteLater);
        out->start();
    });
}

void Toast::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.setPen(QPen(QColor("#2C3446"), 1));
    p.setBrush(QColor(24, 29, 41, 245));
    p.drawRoundedRect(r, 14, 14);
    const QColor dot = m_kind == Kind::Error ? Color::Bad : m_kind == Kind::Good ? Color::Good : Color::Accent2;
    p.setPen(::Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(QPointF(26, r.center().y()), 5, 5);
    QColor halo = dot;
    halo.setAlpha(60);
    p.setBrush(halo);
    p.drawEllipse(QPointF(26, r.center().y()), 9, 9);
    p.setPen(Color::Text);
    p.drawText(r.adjusted(44, 0, -16, 0), ::Qt::AlignVCenter | ::Qt::AlignLeft,
               QFontMetrics(font()).elidedText(m_text, ::Qt::ElideRight, int(r.width()) - 60));
}

} // namespace NeXo2::QtUi
