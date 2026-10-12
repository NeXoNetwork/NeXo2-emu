#pragma once
// Pagina del juego: la imagen de la consola centrada (manteniendo la proporcion), una barra
// arriba que aparece al mover el raton (titulo, pausa, parar, pantalla completa) y un
// indicador de rendimiento en la esquina (F3). Lee el teclado para el mando 1.
#include <QElapsedTimer>
#include <QImage>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include "hle/input.hpp"
#include "widgets.hpp"

namespace NeXo2::QtUi {

class GameView : public QWidget {
    Q_OBJECT
public:
    explicit GameView(QWidget* parent = nullptr);
    // Imagen nueva (RGBA de la consola)
    void SetFrame(const std::vector<u32>& rgba, u32 width, u32 height);
    void ClearFrame();
    void SetTitle(const QString& t);
    void SetPaused(bool p);
    void SetStats(double fps, double mips, const QString& renderer, const QString& pad);
    void SetShowHud(bool on) { m_hud = on; update(); }
    bool ShowHud() const { return m_hud; }
    void SetSmooth(bool on) { m_smooth = on; update(); }
    void SetFullscreenIcon(bool full) { m_full->SetIcon(full ? Icon::ExitFullscreen : Icon::Fullscreen); }
    void SetMessage(const QString& m) { m_message = m; update(); }   // "Cargando...", errores
    HLE::PadInput KeyboardPad() const;
signals:
    void PauseToggled();
    void StopRequested();
    void FullscreenToggled();
    void HudToggled();
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override { m_keys.clear(); }
    void leaveEvent(QEvent*) override { m_barTarget = 0; }
private:
    void PlaceBar();
    QImage m_frame;
    QString m_title, m_message, m_renderer, m_pad;
    double m_fps = 0, m_mips = 0;
    bool m_paused = false, m_hud = true, m_smooth = true;
    QSet<int> m_keys;
    // Barra de arriba: aparece y desaparece suavemente
    qreal m_bar = 1, m_barTarget = 1;
    QElapsedTimer m_lastMove;
    QTimer m_anim;
    QWidget* m_barWidget;
    IconButton* m_pause;
    IconButton* m_stop;
    IconButton* m_full;
    IconButton* m_hudBtn;
    class QLabel* m_titleLabel;
    qreal m_spin = 0;   // animacion de "cargando"
};

} // namespace NeXo2::QtUi
