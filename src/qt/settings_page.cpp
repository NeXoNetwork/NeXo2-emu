#include "settings_page.hpp"

#include <algorithm>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include "theme.hpp"
#include "widgets.hpp"

namespace NeXo2::QtUi {

namespace {
// Icono dentro de un cuadrado redondeado con el degradado suave (cabecera de cada tarjeta)
class CardIcon : public QWidget {
public:
    CardIcon(Icon icon, QWidget* parent = nullptr) : QWidget(parent), m_icon(icon) { setFixedSize(40, 40); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(::Qt::NoPen);
        p.setBrush(AccentGradient(QPointF(0, 0), QPointF(40, 40), 60));
        p.drawRoundedRect(rect(), 11, 11);
        DrawIcon(p, m_icon, QRectF(9, 9, 22, 22), ::Qt::white);
    }
private:
    Icon m_icon;
};

QString ToQ(const std::string& s) { return QString::fromUtf8(s.c_str()); }
} // namespace

QWidget* SettingsPage::Card(QVBoxLayout* into, const QString& title, const QString& subtitle, int icon) {
    auto* card = new QWidget;
    card->setObjectName("card");
    card->setAttribute(::Qt::WA_StyledBackground);
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(22, 20, 22, 14);
    v->setSpacing(6);
    auto* head = new QHBoxLayout;
    head->setSpacing(14);
    head->addWidget(new CardIcon(Icon(icon)));
    auto* tv = new QVBoxLayout;
    tv->setSpacing(1);
    auto* t = new QLabel(title);
    t->setObjectName("sectionTitle");
    auto* st = new QLabel(subtitle);
    st->setObjectName("dim");
    tv->addWidget(t);
    tv->addWidget(st);
    head->addLayout(tv, 1);
    v->addLayout(head);
    v->addSpacing(8);
    into->addWidget(card);
    return card;
}

SettingsPage::SettingsPage(Frontend::Settings& settings, QWidget* parent) : QWidget(parent), m_s(settings) {
    setObjectName("page");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);
    auto* holder = new QWidget;   // centra la columna de ajustes (como mucho 1000 px de ancho)
    holder->setObjectName("page");
    scroll->setWidget(holder);
    auto* hh = new QHBoxLayout(holder);
    hh->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget;
    content->setMaximumWidth(1000);
    hh->addStretch();
    hh->addWidget(content, 100);
    hh->addStretch();
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(36, 30, 36, 36);
    col->setSpacing(16);

    auto* title = new QLabel("Ajustes");
    title->setObjectName("pageTitle");
    auto* sub = new QLabel("Los cambios se guardan solos. Los de gráficos y sistema se aplican al abrir el próximo juego.");
    sub->setObjectName("pageSubtitle");
    col->addWidget(title);
    col->addWidget(sub);
    col->addSpacing(8);

    // Fila: titulo + explicacion a la izquierda, control a la derecha
    auto row = [](QWidget* card, const QString& name, const QString& help, QWidget* control) {
        auto* line = new QFrame;
        line->setFixedHeight(1);
        line->setStyleSheet("background: #1E2533;");
        card->layout()->addWidget(line);
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 10, 0, 10);
        auto* tv = new QVBoxLayout;
        tv->setSpacing(2);
        auto* n = new QLabel(name);
        n->setStyleSheet("font-size: 13px; font-weight: 600;");
        tv->addWidget(n);
        if (!help.isEmpty()) {
            auto* hl = new QLabel(help);
            hl->setObjectName("dim");
            hl->setWordWrap(true);
            tv->addWidget(hl);
        }
        h->addLayout(tv, 1);
        h->addSpacing(20);
        h->addWidget(control, 0, ::Qt::AlignVCenter);
        card->layout()->addWidget(w);
    };
    auto toggle = [this](bool& value, bool restart) {
        auto* t = new ToggleSwitch;
        t->setChecked(value);
        connect(t, &QAbstractButton::toggled, this, [this, &value, restart](bool on) { value = on; emit Changed(restart); });
        m_reload.push_back([t, &value] { QSignalBlocker b(t); t->setChecked(value); });
        return t;
    };
    auto combo = [this](int& value, QStringList items, bool restart) {
        auto* c = new QComboBox;
        c->addItems(items);
        c->setCursor(::Qt::PointingHandCursor);
        c->setCurrentIndex(std::clamp(value, 0, int(items.size()) - 1));
        connect(c, &QComboBox::currentIndexChanged, this, [this, &value, restart](int i) { value = i; emit Changed(restart); });
        m_reload.push_back([c, &value] { QSignalBlocker b(c); c->setCurrentIndex(value); });
        return c;
    };

    // --- Biblioteca ---
    {
        QWidget* card = Card(col, "Biblioteca", "Carpetas donde NeXo busca juegos (además de «games» y «sdmc/switch»)", int(Icon::Folder));
        m_folders = new QListWidget;
        m_folders->setObjectName("folders");
        m_folders->setMinimumHeight(96);
        m_folders->setMaximumHeight(150);
        card->layout()->addWidget(m_folders);
        auto* bh = new QHBoxLayout;
        auto* add = new QPushButton(MakeIcon(Icon::FolderPlus, Color::Text, 18), "  Añadir carpeta");
        auto* remove = new QPushButton(MakeIcon(Icon::Trash, Color::Bad, 18), "  Quitar");
        remove->setObjectName("danger");
        for (auto* b : {add, remove}) b->setCursor(::Qt::PointingHandCursor);
        bh->addWidget(add);
        bh->addWidget(remove);
        bh->addStretch();
        auto* bw = new QWidget;
        bw->setLayout(bh);
        bh->setContentsMargins(0, 8, 0, 6);
        card->layout()->addWidget(bw);
        connect(add, &QPushButton::clicked, this, &SettingsPage::AddFolderRequested);
        connect(remove, &QPushButton::clicked, this, [this] {
            const int r = m_folders->currentRow();
            if (r < 0 || r >= int(m_s.game_dirs.size())) return;
            m_s.game_dirs.erase(m_s.game_dirs.begin() + r);
            Reload();
            emit Changed(false);
            emit FoldersChanged();
        });
    }

    // --- Graficos ---
    {
        QWidget* card = Card(col, "Gráficos", "Cómo se dibuja la imagen de la consola", int(Icon::Gpu));
        m_rendererInfo = new QLabel;
        m_rendererInfo->setObjectName("chip");
        static_cast<QVBoxLayout*>(card->layout())->addWidget(m_rendererInfo, 0, ::Qt::AlignLeft);
        row(card, "Motor de dibujo", "Vulkan usa la tarjeta gráfica (mucho más rápido). Software sirve si Vulkan falla.",
            combo(m_s.renderer, {"Software (CPU)", "Vulkan (tarjeta gráfica)"}, true));
        row(card, "Modo de la consola", "Portátil: 1280×720. Dock (TV): 1920×1080, algunos juegos se ven más nítidos.",
            combo(m_s.docked, {"Portátil", "Dock (TV)"}, true));
        auto* smooth = toggle(m_s.linear_filter, false);
        row(card, "Suavizar al escalar", "Desactívalo para ver píxeles nítidos (juegos retro).", smooth);
        row(card, "GPU en su propio hilo", "La emulación no espera a que termine cada dibujo.", toggle(m_s.gpu_async, true));
    }

    // --- Sistema ---
    {
        QWidget* card = Card(col, "Sistema", "La CPU emulada", int(Icon::Chip));
        row(card, "Recompilador (JIT)", "Traduce el código ARM64 a x86-64. Mucho más rápido que el intérprete.", toggle(m_s.jit, true));
        row(card, "Varios núcleos", "Cada núcleo de la consola corre en un hilo de tu PC.", toggle(m_s.multicore, true));
        row(card, "Caché de instrucciones", "Solo afecta al intérprete.", toggle(m_s.decode_cache, true));
    }

    // --- Sonido ---
    {
        QWidget* card = Card(col, "Sonido", "Salida de audio a 48 kHz estéreo", int(Icon::Volume));
        row(card, "Sonido activado", "", toggle(m_s.audio, false));
        auto* vol = new QWidget;
        auto* vh = new QHBoxLayout(vol);
        vh->setContentsMargins(0, 0, 0, 0);
        auto* slider = new QSlider(::Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setFixedWidth(220);
        slider->setValue(m_s.volume);
        slider->setCursor(::Qt::PointingHandCursor);
        auto* pct = new QLabel(QString("%1 %").arg(m_s.volume));
        pct->setFixedWidth(44);
        pct->setAlignment(::Qt::AlignRight | ::Qt::AlignVCenter);
        vh->addWidget(slider);
        vh->addWidget(pct);
        connect(slider, &QSlider::valueChanged, this, [this, pct](int v) {
            m_s.volume = v;
            pct->setText(QString("%1 %").arg(v));
            emit Changed(false);
        });
        m_reload.push_back([this, slider] { QSignalBlocker b(slider); slider->setValue(m_s.volume); });
        row(card, "Volumen", "", vol);
    }

    // --- Controles ---
    {
        QWidget* card = Card(col, "Controles", "Mando del PC (Xbox, PlayStation…) o teclado", int(Icon::Gamepad));
        m_padInfo = new QLabel;
        m_padInfo->setObjectName("chip");
        static_cast<QVBoxLayout*>(card->layout())->addWidget(m_padInfo, 0, ::Qt::AlignLeft);
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(28);
        grid->setVerticalSpacing(8);
        const char* keys[][2] = {
            {"A / B / X / Y", "X / Z / S / A"}, {"Cruceta", "Flechas"}, {"L / R", "Q / W"}, {"ZL / ZR", "1 / 2"},
            {"+ / −", "Intro / Retroceso"}, {"Stick izquierdo", "T F G H"}, {"Stick derecho", "I J K L"},
            {"Pausa · Pantalla completa", "F5 · F11"},
        };
        int r = 0;
        for (const auto& k : keys) {
            auto* a = new QLabel(k[0]);
            a->setObjectName("dim");
            auto* b = new QLabel(k[1]);
            b->setStyleSheet("font-weight: 600;");
            grid->addWidget(a, r / 2, (r % 2) * 2);
            grid->addWidget(b, r / 2, (r % 2) * 2 + 1);
            ++r;
        }
        auto* gw = new QWidget;
        gw->setLayout(grid);
        grid->setContentsMargins(0, 10, 0, 10);
        card->layout()->addWidget(gw);
    }
    col->addStretch();
    Reload();
}

void SettingsPage::SetInfo(const QString& renderer, const QString& gamepad) {
    m_rendererInfo->setText("Ahora: " + renderer);
    m_padInfo->setText(gamepad.isEmpty() ? "Ningún mando conectado: se usa el teclado" : "Mando conectado: " + gamepad);
}

void SettingsPage::Reload() {
    m_folders->clear();
    for (const auto& d : m_s.game_dirs) m_folders->addItem(new QListWidgetItem(MakeIcon(Icon::Folder, Color::TextDim, 18), ToQ(d)));
    if (m_s.game_dirs.empty()) {
        auto* it = new QListWidgetItem("Ninguna carpeta añadida");
        it->setFlags(::Qt::NoItemFlags);
        m_folders->addItem(it);
    }
    for (auto& f : m_reload) f();
}

} // namespace NeXo2::QtUi
