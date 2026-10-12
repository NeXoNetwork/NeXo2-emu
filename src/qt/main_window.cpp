#include "main_window.hpp"

#include <algorithm>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include "game_view.hpp"
#include "library_page.hpp"
#include "settings_page.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#ifndef NEXO2_VERSION
#define NEXO2_VERSION "0.0.2"
#endif

namespace NeXo2::QtUi {

namespace {
enum Page { PAGE_LIBRARY, PAGE_GAME, PAGE_SETTINGS, PAGE_ABOUT };
QString ToQ(const std::string& s) { return QString::fromUtf8(s.c_str()); }
std::string ToStd(const QString& s) { return s.toUtf8().toStdString(); }

// Fondo de la pagina "Acerca de": manchas de color difuminadas
class GlowBackground : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Color::Bg);
        p.setRenderHint(QPainter::Antialiasing);
        auto blob = [&](QPointF c, qreal r, QColor col) {
            QRadialGradient g(c, r);
            col.setAlpha(70);
            g.setColorAt(0, col);
            col.setAlpha(0);
            g.setColorAt(1, col);
            p.setPen(::Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(c, r, r);
        };
        blob(QPointF(width() * 0.25, height() * 0.3), width() * 0.35, Color::Accent);
        blob(QPointF(width() * 0.8, height() * 0.75), width() * 0.3, Color::Accent2);
    }
};
} // namespace

MainWindow::MainWindow(EmuHost& host) : m_host(host) {
    setWindowTitle("NeXo 2");
    setMinimumSize(1100, 680);
    resize(1440, 880);
    setAcceptDrops(true);

    auto* central = new QWidget;
    auto* h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    h->addWidget(BuildSidebar());

    m_pages = new QStackedWidget;
    m_library = new LibraryPage;
    m_game = new GameView;
    m_settings = new SettingsPage(m_host.Settings());
    m_pages->addWidget(m_library);
    m_pages->addWidget(m_game);
    m_pages->addWidget(m_settings);
    m_pages->addWidget(BuildAbout());
    h->addWidget(m_pages, 1);
    setCentralWidget(central);

    // Biblioteca
    connect(m_library, &LibraryPage::PlayRequested, this, &MainWindow::Launch);
    connect(m_library, &LibraryPage::AddFolderRequested, this, &MainWindow::AddFolder);
    connect(m_library, &LibraryPage::OpenFileRequested, this, &MainWindow::OpenFile);
    connect(m_library, &LibraryPage::RefreshRequested, this, [this] { Rescan(); Toast::Show(this, "Biblioteca actualizada", Toast::Kind::Good); });
    // Juego
    connect(m_game, &GameView::PauseToggled, this, &MainWindow::TogglePause);
    connect(m_game, &GameView::StopRequested, this, &MainWindow::StopGame);
    connect(m_game, &GameView::FullscreenToggled, this, [this] { SetFullscreen(!isFullScreen()); });
    connect(m_game, &GameView::HudToggled, this, [this] { m_game->SetShowHud(!m_game->ShowHud()); });
    // Ajustes
    connect(m_settings, &SettingsPage::Changed, this, [this](bool restart) {
        m_host.SaveSettings();
        m_game->SetSmooth(m_host.Settings().linear_filter);
        if (!restart) m_host.ApplySettingsLight();
    });
    connect(m_settings, &SettingsPage::FoldersChanged, this, &MainWindow::Rescan);
    connect(m_settings, &SettingsPage::AddFolderRequested, this, &MainWindow::AddFolder);

    // Atajos
    auto shortcut = [this](const QKeySequence& k, auto fn) {
        auto* s = new QShortcut(k, this);
        s->setContext(::Qt::ApplicationShortcut);
        connect(s, &QShortcut::activated, this, fn);
    };
    shortcut(QKeySequence("Ctrl+O"), [this] { OpenFile(); });
    shortcut(QKeySequence("Ctrl+F"), [this] { ShowPage(PAGE_LIBRARY); m_library->FocusSearch(); });
    shortcut(QKeySequence("Ctrl+,"), [this] { ShowPage(PAGE_SETTINGS); });

    m_game->SetSmooth(m_host.Settings().linear_filter);
    m_timer.setTimerType(::Qt::PreciseTimer);
    m_timer.setInterval(8);
    connect(&m_timer, &QTimer::timeout, this, &MainWindow::Tick);
    m_timer.start();

    Rescan();
    ShowPage(PAGE_LIBRARY);
    UpdateStatus();
}

QWidget* MainWindow::BuildSidebar() {
    m_sidebar = new QWidget;
    m_sidebar->setObjectName("sidebar");
    m_sidebar->setAttribute(::Qt::WA_StyledBackground);
    m_sidebar->setFixedWidth(232);
    auto* v = new QVBoxLayout(m_sidebar);
    v->setContentsMargins(0, 10, 0, 18);
    v->setSpacing(4);
    v->addWidget(new Logo);
    v->addSpacing(18);

    auto* menu = new QLabel("MENÚ");
    menu->setObjectName("faint");
    menu->setStyleSheet("font-size: 10px; font-weight: 700; letter-spacing: 1.5px; padding-left: 26px;");
    v->addWidget(menu);
    v->addSpacing(4);
    m_navLibrary = new NavButton(Icon::Library, "Biblioteca");
    m_navGame = new NavButton(Icon::Play, "Jugando ahora");
    m_navSettings = new NavButton(Icon::Settings, "Ajustes");
    m_navAbout = new NavButton(Icon::Info, "Acerca de");
    m_navGame->setEnabled(false);
    v->addWidget(m_navLibrary);
    v->addWidget(m_navGame);
    v->addWidget(m_navSettings);
    v->addWidget(m_navAbout);
    connect(m_navLibrary, &QAbstractButton::clicked, this, [this] { ShowPage(PAGE_LIBRARY); });
    connect(m_navGame, &QAbstractButton::clicked, this, [this] { ShowPage(PAGE_GAME); });
    connect(m_navSettings, &QAbstractButton::clicked, this, [this] { ShowPage(PAGE_SETTINGS); });
    connect(m_navAbout, &QAbstractButton::clicked, this, [this] { ShowPage(PAGE_ABOUT); });
    v->addStretch();

    // Estado abajo: motor de dibujo y mando
    auto status = [&](Icon icon, QLabel*& label) {
        auto* w = new QWidget;
        auto* hl = new QHBoxLayout(w);
        hl->setContentsMargins(24, 4, 18, 4);
        hl->setSpacing(10);
        auto* ic = new QLabel;
        ic->setPixmap(MakeIcon(icon, Color::TextDim, 18).pixmap(18, 18));
        label = new QLabel;
        label->setObjectName("dim");
        label->setStyleSheet("font-size: 12px;");
        hl->addWidget(ic);
        hl->addWidget(label, 1);
        v->addWidget(w);
    };
    status(Icon::Gpu, m_statusGpu);
    status(Icon::Gamepad, m_statusPad);
    return m_sidebar;
}

QWidget* MainWindow::BuildAbout() {
    auto* page = new GlowBackground;
    auto* v = new QVBoxLayout(page);
    v->addStretch();
    auto* card = new QWidget;
    card->setObjectName("card");
    card->setAttribute(::Qt::WA_StyledBackground);
    card->setFixedWidth(560);
    auto* cv = new QVBoxLayout(card);
    cv->setContentsMargins(36, 30, 36, 30);
    cv->setSpacing(10);
    auto* logo = new Logo;
    logo->setFixedWidth(200);
    cv->addWidget(logo, 0, ::Qt::AlignHCenter);
    auto* ver = new QLabel(QString("Versión ") + NEXO2_VERSION);
    ver->setObjectName("chip");
    ver->setAlignment(::Qt::AlignCenter);
    cv->addWidget(ver, 0, ::Qt::AlignHCenter);
    cv->addSpacing(6);
    auto* text = new QLabel(
        "Emulador de Nintendo Switch y Switch 2 escrito desde cero en C++20.<br><br>"
        "NeXo no incluye juegos, claves ni firmware: usa los volcados de tu propia consola "
        "(pon tus <b>prod.keys</b> en la carpeta <b>keys</b>).<br><br>"
        "<span style='color:#8C95A8'>CPU ARM64 con recompilador · GPU Maxwell con Vulkan · "
        "audio audren · servicios HLE de Horizon</span>");
    text->setWordWrap(true);
    text->setAlignment(::Qt::AlignCenter);
    text->setTextFormat(::Qt::RichText);
    cv->addWidget(text);
    cv->addSpacing(10);
    auto* folder = new QPushButton(MakeIcon(Icon::Folder, Color::Text, 18), "  Abrir carpeta de NeXo");
    folder->setCursor(::Qt::PointingHandCursor);
    connect(folder, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(ToQ(m_host.BaseDir()))); });
    cv->addWidget(folder, 0, ::Qt::AlignHCenter);
    v->addWidget(card, 0, ::Qt::AlignHCenter);
    v->addStretch();
    return page;
}

void MainWindow::ShowPage(int index) {
    m_pages->setCurrentIndex(index);
    NavButton* nav[] = {m_navLibrary, m_navGame, m_navSettings, m_navAbout};
    nav[index]->setChecked(true);
    if (index == PAGE_SETTINGS) {
        m_settings->Reload();
        m_settings->SetInfo(ToQ(m_host.RendererName()), ToQ(m_host.GamepadName()));
    }
    if (index == PAGE_GAME) m_game->setFocus();
}

void MainWindow::ShowNamedPage(const QString& name) {
    if (name == "ajustes") ShowPage(PAGE_SETTINGS);
    else if (name == "acerca") ShowPage(PAGE_ABOUT);
    else if (name == "detalles") m_library->SelectFirst();
    else if (name == "juego" || name == "pausa") {   // imagen de prueba (un degradado) para ver la pagina del juego
        std::vector<u32> img(1280 * 720);
        for (u32 y = 0; y < 720; ++y)
            for (u32 x = 0; x < 1280; ++x)
                img[y * 1280 + x] = 0xFF000000u | (u32(40 + y * 120 / 720)) | (u32(60 + x * 100 / 1280) << 8) | (u32(140 + x * 80 / 1280) << 16);
        m_game->SetTitle("Imagen de prueba");
        m_game->SetFrame(img, 1280, 720);
        m_game->SetStats(59.9, 1840, "Vulkan (NVIDIA GeForce RTX 4070)", "Xbox 360 Controller");
        m_game->SetPaused(name == "pausa");
        m_navGame->setEnabled(true);
        ShowPage(PAGE_GAME);
    }
}

void MainWindow::Rescan() {
    QApplication::setOverrideCursor(::Qt::WaitCursor);
    m_library->SetGames(m_host.ScanLibrary());
    QApplication::restoreOverrideCursor();
}

void MainWindow::AddFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Añadir carpeta de juegos");
    if (dir.isEmpty()) return;
    auto& dirs = m_host.Settings().game_dirs;
    const std::string d = ToStd(QDir::toNativeSeparators(dir));
    if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    m_host.SaveSettings();
    m_settings->Reload();
    Rescan();
    Toast::Show(this, "Carpeta añadida: " + dir, Toast::Kind::Good);
}

void MainWindow::OpenFile() {
    const QString file = QFileDialog::getOpenFileName(this, "Abrir juego o programa", ToQ(m_host.BaseDir()),
        "Juegos de Switch (*.nsp *.xci *.nro *.npdm);;Todos los archivos (*)");
    if (!file.isEmpty()) Launch(file);
}

void MainWindow::Launch(const QString& path) {
    std::string error;
    m_game->ClearFrame();
    m_seenFrame = 0;
    if (!m_host.Launch(ToStd(path), error)) {
        Toast::Show(this, "No se pudo abrir: " + ToQ(error), Toast::Kind::Error, 6000);
        return;
    }
    m_reportedHalt = false;
    m_game->SetTitle(ToQ(m_host.Title()));
    m_game->SetMessage("");
    m_game->SetPaused(false);
    setWindowTitle("NeXo 2  —  " + ToQ(m_host.Title()));
    m_navGame->setEnabled(true);
    ShowPage(PAGE_GAME);
}

void MainWindow::StopGame() {
    m_host.Stop();
    if (isFullScreen()) SetFullscreen(false);
    m_game->ClearFrame();
    m_navGame->setEnabled(false);
    setWindowTitle("NeXo 2");
    ShowPage(PAGE_LIBRARY);
}

void MainWindow::TogglePause() {
    if (!m_host.InGame()) return;
    m_host.SetPaused(!m_host.Paused());
    m_game->SetPaused(m_host.Paused());
}

void MainWindow::SetFullscreen(bool on) {
    if (on == isFullScreen()) return;
    m_sidebar->setVisible(!on);
    if (on) {
        m_wasMaximized = isMaximized();
        showFullScreen();
    } else if (m_wasMaximized) {
        showMaximized();
    } else {
        showNormal();
    }
    m_game->SetFullscreenIcon(on);
    m_game->setFocus();
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == ::Qt::Key_Escape && isFullScreen()) { SetFullscreen(false); return; }
    if (e->key() == ::Qt::Key_F11 && m_host.InGame()) { SetFullscreen(!isFullScreen()); return; }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::Tick() {
    const bool playing = m_pages->currentIndex() == PAGE_GAME;
    m_host.Tick(playing ? m_game->KeyboardPad() : HLE::PadInput{});
    if (!m_host.InGame()) return;
    u32 w = 0, hgt = 0;
    if (m_host.GrabFrame(m_seenFrame, m_frameBuf, w, hgt)) m_game->SetFrame(m_frameBuf, w, hgt);
    if (++m_tickCount % 30 == 0) {   // ~4 veces por segundo
        UpdateStatus();
        m_game->SetStats(m_host.Fps(), m_host.Mips(), ToQ(m_host.RendererName()), ToQ(m_host.GamepadName()));
        // El juego se paro solo (error, o termino): avisar una vez
        if (!m_reportedHalt && !m_host.Paused() && m_host.InGame()) {
            const std::string reason = m_host.HaltReason();
            if (!reason.empty()) {
                m_reportedHalt = true;
                m_game->SetMessage("El juego se ha detenido: " + ToQ(reason));
                Toast::Show(this, "El juego se ha detenido: " + ToQ(reason), Toast::Kind::Error, 8000);
            }
        }
    }
}

void MainWindow::UpdateStatus() {
    QString gpu = ToQ(m_host.RendererName());
    if (gpu.startsWith("Vulkan (")) gpu = gpu.mid(8, gpu.size() - 9);   // solo el nombre de la tarjeta
    m_statusGpu->setText(QFontMetrics(m_statusGpu->font()).elidedText(gpu, ::Qt::ElideRight, 170));
    m_statusGpu->setToolTip(ToQ(m_host.RendererName()));
    const QString pad = ToQ(m_host.GamepadName());
    m_statusPad->setText(pad.isEmpty() ? "Teclado" : QFontMetrics(m_statusPad->font()).elidedText(pad, ::Qt::ElideRight, 170));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    m_timer.stop();
    m_host.Stop();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const auto urls = e->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile()) Launch(urls.first().toLocalFile());
}

} // namespace NeXo2::QtUi
