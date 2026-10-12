#pragma once
// Piezas pequenas de la interfaz Qt: interruptor animado, boton de la barra lateral,
// boton con icono, aviso flotante ("toast") y el logo.
#include <QAbstractButton>
#include <QLabel>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>
#include "theme.hpp"

namespace NeXo2::QtUi {

// Interruptor encendido/apagado con animacion (como los de los moviles)
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal pos READ Pos WRITE SetPos)
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {46, 26}; }
    qreal Pos() const { return m_pos; }
    void SetPos(qreal p) { m_pos = p; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; update(); }
private:
    qreal m_pos = 0;
    bool m_hover = false;
    QPropertyAnimation m_anim;
};

// Boton de la barra lateral: icono + texto, con una barra de color si esta elegido
class NavButton : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal glow READ Glow WRITE SetGlow)
public:
    NavButton(Icon icon, const QString& text, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {200, 44}; }
    qreal Glow() const { return m_glow; }
    void SetGlow(qreal g) { m_glow = g; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    Icon m_icon;
    qreal m_glow = 0;   // 0..1 al pasar el raton
    QPropertyAnimation m_anim;
};

// Boton redondo solo con icono (barra del juego, cabeceras)
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    IconButton(Icon icon, const QString& tooltip, QWidget* parent = nullptr, int size = 38);
    void SetIcon(Icon icon) { m_icon = icon; update(); }
    void SetChecked(bool on) { setCheckable(true); setChecked(on); update(); }
    QSize sizeHint() const override { return {m_size, m_size}; }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; update(); }
private:
    Icon m_icon;
    int m_size;
    bool m_hover = false;
};

// Logo "NeXo 2" con el degradado de la marca
class Logo : public QWidget {
public:
    explicit Logo(QWidget* parent = nullptr) : QWidget(parent) { setFixedHeight(64); }
protected:
    void paintEvent(QPaintEvent*) override;
};

// Aviso que aparece abajo, se queda unos segundos y se desvanece
class Toast : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ Opacity WRITE SetOpacity)
public:
    enum class Kind { Info, Good, Error };
    static void Show(QWidget* host, const QString& text, Kind kind = Kind::Info, int ms = 3200);
    qreal Opacity() const { return m_opacity; }
    void SetOpacity(qreal o) { m_opacity = o; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    Toast(QWidget* host, const QString& text, Kind kind);
    QString m_text;
    Kind m_kind;
    qreal m_opacity = 0;
};

} // namespace NeXo2::QtUi
