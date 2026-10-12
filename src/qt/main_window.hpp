#pragma once
// Ventana principal de NeXo2-Qt: barra lateral (Biblioteca, Jugando, Ajustes, Acerca de)
// y las paginas. Un temporizador (~120 Hz) pasa los botones al juego y recoge las imagenes.
#include <QMainWindow>
#include <QTimer>
#include <memory>
#include "emu_host.hpp"

class QLabel;
class QStackedWidget;

namespace NeXo2::QtUi {

class GameView;
class LibraryPage;
class NavButton;
class SettingsPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(EmuHost& host);
    void Launch(const QString& path);
    void ShowNamedPage(const QString& name);   // "ajustes", "acerca", "detalles" (capturas)
protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    void Rescan();
    void AddFolder();
    void OpenFile();
    void StopGame();
    void TogglePause();
    void SetFullscreen(bool on);
    void ShowPage(int index);
    void Tick();
    void UpdateStatus();
    QWidget* BuildSidebar();
    QWidget* BuildAbout();

    EmuHost& m_host;
    QWidget* m_sidebar = nullptr;
    QStackedWidget* m_pages = nullptr;
    LibraryPage* m_library = nullptr;
    GameView* m_game = nullptr;
    SettingsPage* m_settings = nullptr;
    NavButton* m_navLibrary = nullptr;
    NavButton* m_navGame = nullptr;
    NavButton* m_navSettings = nullptr;
    NavButton* m_navAbout = nullptr;
    QLabel* m_statusGpu = nullptr;
    QLabel* m_statusPad = nullptr;
    QTimer m_timer;
    u64 m_seenFrame = 0;
    std::vector<u32> m_frameBuf;
    int m_tickCount = 0;
    bool m_reportedHalt = false;
    bool m_wasMaximized = false;
};

} // namespace NeXo2::QtUi
