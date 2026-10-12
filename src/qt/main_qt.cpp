// NeXo2-Qt.exe: la interfaz con Qt (biblioteca, ajustes, juego). NeXo2.exe (SDL + ImGui)
// sigue existiendo con las ventanas de depuracion. Ver docs/07-nexo-internals/frontend-qt.md.
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "common/logger.hpp"
#include "frontend/crash_report.hpp"
#include "hle/kernel.hpp"
#include "emu_host.hpp"
#include "main_window.hpp"
#include "theme.hpp"

using namespace NeXo2;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("NeXo 2");
    QApplication::setOrganizationName("NeXo");

    // Todo junto al ejecutable: nexo2-qt.log, nexo2.ini, sdmc, games, keys
    std::string base = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toUtf8().toStdString();
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += QDir::separator().toLatin1();

    static std::string logPath = base + "nexo2-qt.log";
    {
        std::error_code ec;
        const auto p = Frontend::Utf8ToPath(logPath);
        if (std::filesystem::exists(p, ec)) std::filesystem::rename(p, p.parent_path() / "nexo2-qt-anterior.log", ec);
        Common::Logger::EnableFile(logPath.c_str());
    }

    QtUi::ApplyTheme(app);
    QtUi::EmuHost host(base);
    Frontend::InstallCrashReport(logPath, [&host] {
        std::string s = "[CRASH] Programa: " + host.Sys().GetProgramName();
        for (int c = 0; c < HLE::NUM_CORES; ++c) {
            auto* cpu = host.Sys().GetKernel().CoreCpu(c);
            if (!cpu) continue;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "\n[CRASH] Nucleo %d: PC 0x%llX LR 0x%llX", c,
                          (unsigned long long)cpu->GetState().pc, (unsigned long long)cpu->GetState().x[30]);
            s += buf;
        }
        return s;
    });

    QtUi::MainWindow window(host);
    QtUi::DarkTitleBar(&window);
    window.show();
    // NEXO2_QT_SHOT=archivo.png[:pagina]: captura de la ventana y salir (para la documentacion)
    if (const char* shot = std::getenv("NEXO2_QT_SHOT")) {
        const QString spec = QString::fromLocal8Bit(shot);
        const QString file = spec.section(':', 0, 0), page = spec.section(':', 1);
        QTimer::singleShot(1200, &window, [&window, file, page] {
            window.ShowNamedPage(page);
            QTimer::singleShot(900, &window, [&window, file] {
                window.grab().save(file);
                QApplication::quit();
            });
        });
    }
    // Un juego en la linea de comandos: se abre directamente
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        QTimer::singleShot(0, &window, [&window, path] { window.Launch(path); });
    }
    return app.exec();
}
