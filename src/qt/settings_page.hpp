#pragma once
// Pagina "Ajustes": tarjetas por tema (Biblioteca, Graficos, Sistema, Sonido, Controles).
// Cada cambio se guarda en nexo2.ini al momento (los comparte con NeXo2.exe).
#include <QWidget>
#include <functional>
#include <vector>
#include "frontend/settings.hpp"

class QLabel;
class QListWidget;
class QVBoxLayout;

namespace NeXo2::QtUi {

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    SettingsPage(Frontend::Settings& settings, QWidget* parent = nullptr);
    void SetInfo(const QString& renderer, const QString& gamepad);   // linea de estado de cada tarjeta
    void Reload();   // vuelve a leer los valores (si cambiaron en otro sitio)
signals:
    void Changed(bool needs_restart);   // needs_restart: se nota al abrir el siguiente juego
    void FoldersChanged();
    void AddFolderRequested();
private:
    QWidget* Card(QVBoxLayout* into, const QString& title, const QString& subtitle, int icon);
    Frontend::Settings& m_s;
    QListWidget* m_folders = nullptr;
    QLabel* m_rendererInfo = nullptr;
    QLabel* m_padInfo = nullptr;
    std::vector<std::function<void()>> m_reload;
};

} // namespace NeXo2::QtUi
