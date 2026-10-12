#include "library_page.hpp"

#include <QDesktopServices>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include "theme.hpp"
#include "widgets.hpp"

namespace NeXo2::QtUi {

namespace {
QString ToQ(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }

QString HumanSize(u64 bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; ++u; }
    return QString::number(v, 'f', u ? 1 : 0) + " " + units[u];
}

// Caratula de reserva: degradado (color segun el titulo) con las iniciales
QImage FallbackCover(const QString& title, int size) {
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    const uint h = qHash(title);
    const QColor a = QColor::fromHsv(int(h % 360), 150, 120), b = QColor::fromHsv(int((h / 360 + 200) % 360), 170, 60);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, size, size);
    g.setColorAt(0, a);
    g.setColorAt(1, b);
    p.fillRect(img.rect(), g);
    QString initials;
    for (const QString& w : title.split(' ', ::Qt::SkipEmptyParts)) {
        if (w[0].isLetterOrNumber()) initials += w[0].toUpper();
        if (initials.size() == 2) break;
    }
    if (initials.isEmpty()) initials = "?";
    QFont f;
    f.setPixelSize(size / 3);
    f.setWeight(QFont::Bold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 220));
    p.drawText(img.rect(), ::Qt::AlignCenter, initials);
    return img;
}

QString FormatOf(const Frontend::GameEntry& e) {
    if (e.is_folder) return "Carpeta";
    const QString ext = QFileInfo(ToQ(e.file_name)).suffix().toUpper();
    return ext.isEmpty() ? "Juego" : ext;
}

// Imagen de cabecera del panel de detalles: la caratula desenfocada de fondo y nitida delante
QPixmap MakeHero(const QImage& cover, int w, int h, qreal dpr) {
    QPixmap pm(int(w * dpr), int(h * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(::Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, w, h), 18, 18);
    p.setClipPath(clip);
    // Desenfoque barato: reducir mucho y volver a ampliar suavizando
    const QImage tiny = cover.scaled(12, 12, ::Qt::IgnoreAspectRatio, ::Qt::SmoothTransformation);
    p.drawImage(QRectF(-20, -20, w + 40, h + 40), tiny.scaled(w / 2, h / 2, ::Qt::IgnoreAspectRatio, ::Qt::SmoothTransformation));
    QLinearGradient shade(0, 0, 0, h);
    shade.setColorAt(0, QColor(11, 14, 20, 90));
    shade.setColorAt(1, QColor(11, 14, 20, 235));
    p.fillRect(QRectF(0, 0, w, h), shade);
    const int cs = std::min(w, h) - 70;
    const QRectF cr((w - cs) / 2.0, 26, cs, cs);
    p.setClipping(false);
    // Sombra suave debajo de la caratula
    for (int i = 6; i > 0; --i) {
        p.setPen(::Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 14));
        p.drawRoundedRect(cr.adjusted(-i, -i + 6, i, i + 6), 16 + i, 16 + i);
    }
    QPainterPath cp;
    cp.addRoundedRect(cr, 16, 16);
    p.setClipPath(cp);
    p.drawImage(cr, cover);
    p.setClipping(false);
    p.setPen(QPen(QColor(255, 255, 255, 30), 1));
    p.setBrush(::Qt::NoBrush);
    p.drawRoundedRect(cr, 16, 16);
    return pm;
}
} // namespace

// ---- Modelo -----------------------------------------------------------------------
void LibraryModel::SetGames(std::vector<LibraryGame> games) {
    beginResetModel();
    m_games = std::move(games);
    endResetModel();
}

QVariant LibraryModel::data(const QModelIndex& index, int role) const {
    const LibraryGame* g = Game(index.row());
    if (!g) return {};
    if (role == ::Qt::DisplayRole) return ToQ(g->entry.title);
    if (role == SearchRole) return ToQ(g->entry.title + " " + g->entry.author + " " + g->entry.file_name);
    if (role == GameRole) return index.row();
    return {};
}

// ---- Tarjetas -----------------------------------------------------------------------
void CardDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const {
    auto* proxy = qobject_cast<const QSortFilterProxyModel*>(index.model());
    const QModelIndex src = proxy ? proxy->mapToSource(index) : index;
    auto* model = static_cast<const LibraryModel*>(src.model());
    const LibraryGame* g = model->Game(src.row());
    if (!g) return;
    const bool hover = opt.state & QStyle::State_MouseOver;
    const bool sel = opt.state & QStyle::State_Selected;

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    // Tarjeta de ancho fijo, centrada en su celda (la cuadricula reparte el ancho sobrante)
    QRectF card(opt.rect.center().x() - CARD_W / 2.0 + 0.5, opt.rect.top() + GAP, CARD_W, CARD_H);
    if (hover) card.translate(0, -3);   // la tarjeta "se levanta" con el raton encima

    // Sombra y fondo
    if (hover || sel) {
        for (int i = 8; i > 0; --i) {
            QColor c = sel ? Color::Accent : QColor(0, 0, 0);
            c.setAlpha(sel ? 7 : 12);
            p->setPen(::Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(card.adjusted(-i, -i + 4, i, i + 4), 16 + i, 16 + i);
        }
    }
    p->setPen(::Qt::NoPen);
    p->setBrush(hover ? Color::SurfaceHi : Color::Surface);
    p->drawRoundedRect(card, 16, 16);

    // Caratula cuadrada
    const QRectF cover(card.left() + 10, card.top() + 10, card.width() - 20, card.width() - 20);
    QPainterPath clip;
    clip.addRoundedRect(cover, 12, 12);
    p->setClipPath(clip);
    p->drawImage(cover, g->cover);
    if (hover) p->fillRect(cover, QColor(255, 255, 255, 10));
    p->setClipping(false);

    // Etiqueta del formato (arriba a la derecha)
    QFont small = opt.font;
    small.setPointSizeF(7.5);
    small.setWeight(QFont::Bold);
    small.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    p->setFont(small);
    const QString fmt = g->format.toUpper();
    const qreal fw = QFontMetricsF(small).horizontalAdvance(fmt) + 14;
    const QRectF badge(cover.right() - fw - 8, cover.top() + 8, fw, 20);
    p->setBrush(QColor(8, 10, 16, 190));
    p->drawRoundedRect(badge, 7, 7);
    p->setPen(Color::Text);
    p->drawText(badge, ::Qt::AlignCenter, fmt);

    // Boton de jugar al pasar el raton
    if (hover) {
        const QPointF c(cover.right() - 26, cover.bottom() - 26);
        p->setPen(::Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 80));
        p->drawEllipse(c + QPointF(0, 2), 21, 21);
        p->setBrush(AccentGradient(c - QPointF(20, 20), c + QPointF(20, 20)));
        p->drawEllipse(c, 20, 20);
        DrawIcon(*p, Icon::Play, QRectF(c.x() - 9, c.y() - 9, 18, 18), ::Qt::white);
    }

    // Titulo y autor
    QFont title = opt.font;
    title.setPointSizeF(10.5);
    title.setWeight(QFont::DemiBold);
    p->setFont(title);
    p->setPen(Color::Text);
    const QRectF tr(card.left() + 14, cover.bottom() + 12, card.width() - 28, 20);
    p->drawText(tr, ::Qt::AlignLeft | ::Qt::AlignVCenter,
                QFontMetrics(title).elidedText(QString::fromUtf8(g->entry.title.c_str()), ::Qt::ElideRight, int(tr.width())));
    QFont sub = opt.font;
    sub.setPointSizeF(9);
    p->setFont(sub);
    p->setPen(Color::TextDim);
    QString author = QString::fromUtf8(g->entry.author.c_str());
    if (author.isEmpty()) author = g->format == "Carpeta" ? "Juego extraído" : "Desconocido";
    if (!g->entry.version.empty()) author += "  ·  v" + QString::fromUtf8(g->entry.version.c_str());
    p->drawText(tr.translated(0, 21), ::Qt::AlignLeft | ::Qt::AlignVCenter,
                QFontMetrics(sub).elidedText(author, ::Qt::ElideRight, int(tr.width())));

    if (sel) {
        p->setPen(QPen(QBrush(AccentGradient(card.topLeft(), card.bottomRight())), 2));
        p->setBrush(::Qt::NoBrush);
        p->drawRoundedRect(card.adjusted(1, 1, -1, -1), 15, 15);
    }
    p->restore();
}

// ---- Pagina -----------------------------------------------------------------------------
LibraryPage::LibraryPage(QWidget* parent) : QWidget(parent) {
    setObjectName("page");
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- Columna principal ---
    auto* main = new QWidget;
    auto* col = new QVBoxLayout(main);
    col->setContentsMargins(36, 30, 26, 0);
    col->setSpacing(18);

    auto* header = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto* title = new QLabel("Biblioteca");
    title->setObjectName("pageTitle");
    m_count = new QLabel;
    m_count->setObjectName("pageSubtitle");
    titles->addWidget(title);
    titles->addWidget(m_count);
    header->addLayout(titles);
    header->addStretch();
    auto* refresh = new IconButton(Icon::Refresh, "Volver a buscar juegos");
    auto* openFile = new QPushButton(MakeIcon(Icon::File, Color::Text, 18), "  Abrir archivo");
    auto* addFolder = new QPushButton(MakeIcon(Icon::FolderPlus, ::Qt::white, 18), "  Añadir carpeta");
    addFolder->setObjectName("primary");
    for (auto* b : {openFile, addFolder}) b->setCursor(::Qt::PointingHandCursor);
    header->addWidget(refresh);
    header->addSpacing(6);
    header->addWidget(openFile);
    header->addWidget(addFolder);
    col->addLayout(header);
    connect(refresh, &QAbstractButton::clicked, this, &LibraryPage::RefreshRequested);
    connect(openFile, &QPushButton::clicked, this, &LibraryPage::OpenFileRequested);
    connect(addFolder, &QPushButton::clicked, this, &LibraryPage::AddFolderRequested);

    // Buscador con su lupa
    m_search = new QLineEdit;
    m_search->setPlaceholderText("Buscar por nombre, autor o archivo…   (Ctrl+F)");
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(520);
    m_search->addAction(MakeIcon(Icon::Search, Color::TextDim, 18), QLineEdit::LeadingPosition);
    m_search->setStyleSheet("QLineEdit { padding-left: 10px; }");
    col->addWidget(m_search);

    m_model = new LibraryModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterRole(LibraryModel::SearchRole);
    m_proxy->setFilterCaseSensitivity(::Qt::CaseInsensitive);
    m_proxy->setSortCaseSensitivity(::Qt::CaseInsensitive);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_proxy->setFilterFixedString(t);
        UpdateCount();
    });

    m_view = new QListView;
    m_view->setObjectName("library");
    m_view->setModel(m_proxy);
    m_view->setItemDelegate(new CardDelegate(m_view));
    m_view->setViewMode(QListView::IconMode);
    m_view->setResizeMode(QListView::Adjust);
    m_view->setMovement(QListView::Static);
    m_view->setUniformItemSizes(true);
    m_view->setMouseTracking(true);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setContextMenuPolicy(::Qt::CustomContextMenu);
    m_view->viewport()->setAttribute(::Qt::WA_Hover);
    m_view->setCursor(::Qt::PointingHandCursor);

    // Las columnas ocupan todo el ancho: el hueco que sobra se reparte entre ellas
    m_view->viewport()->installEventFilter(this);

    // Sin juegos: un mensaje grande con los pasos
    auto* empty = new QWidget;
    auto* ev = new QVBoxLayout(empty);
    ev->addStretch();
    auto* emptyIcon = new QLabel;
    emptyIcon->setPixmap(MakeIcon(Icon::Gamepad, Color::Accent, 72).pixmap(72, 72));
    emptyIcon->setAlignment(::Qt::AlignCenter);
    auto* emptyTitle = new QLabel("Todavía no hay juegos");
    emptyTitle->setObjectName("sectionTitle");
    emptyTitle->setStyleSheet("font-size: 20px;");
    emptyTitle->setAlignment(::Qt::AlignCenter);
    auto* emptyText = new QLabel("Añade la carpeta donde guardas tus .nsp, .xci o .nro,\n"
                                 "o copia tus juegos en la carpeta «games» junto a NeXo.\n"
                                 "Para los .nsp y .xci pon tus claves en la carpeta «keys».");
    emptyText->setObjectName("dim");
    emptyText->setAlignment(::Qt::AlignCenter);
    auto* emptyBtn = new QPushButton(MakeIcon(Icon::FolderPlus, ::Qt::white, 18), "  Añadir carpeta");
    emptyBtn->setObjectName("primary");
    emptyBtn->setCursor(::Qt::PointingHandCursor);
    connect(emptyBtn, &QPushButton::clicked, this, &LibraryPage::AddFolderRequested);
    ev->addWidget(emptyIcon);
    ev->addSpacing(8);
    ev->addWidget(emptyTitle);
    ev->addWidget(emptyText);
    ev->addSpacing(14);
    ev->addWidget(emptyBtn, 0, ::Qt::AlignHCenter);
    ev->addStretch(2);

    m_body = new QStackedWidget;
    m_body->addWidget(m_view);
    m_body->addWidget(empty);
    col->addWidget(m_body, 1);
    root->addWidget(main, 1);

    // --- Panel de detalles ---
    m_details = new QWidget;
    m_details->setFixedWidth(330);
    m_details->setObjectName("details");
    m_details->setAttribute(::Qt::WA_StyledBackground);
    m_details->setStyleSheet("#details { background: #0F131B; border-left: 1px solid #1A202C; }");
    auto* dv = new QVBoxLayout(m_details);
    dv->setContentsMargins(22, 30, 22, 26);
    dv->setSpacing(10);
    m_dCover = new QLabel;
        m_dTitle = new QLabel;
    m_dTitle->setWordWrap(true);
    m_dTitle->setStyleSheet("font-size: 19px; font-weight: 700;");
    m_dAuthor = new QLabel;
    m_dAuthor->setStyleSheet("color: #8C95A8; font-size: 13px;");
    m_dMeta = new QLabel;
    m_dMeta->setTextFormat(::Qt::RichText);
    m_dMeta->setStyleSheet("color: #B5BDCC; font-size: 12px;");
    m_dPath = new QLabel;
    m_dPath->setWordWrap(true);
    m_dPath->setStyleSheet("color: #5B6476; font-size: 11px;");
    m_dPath->setTextInteractionFlags(::Qt::TextSelectableByMouse);
    m_dPlay = new QPushButton(MakeIcon(Icon::Play, ::Qt::white, 18), "  Jugar");
    m_dPlay->setObjectName("primary");
    m_dPlay->setMinimumHeight(46);
    m_dPlay->setCursor(::Qt::PointingHandCursor);
    m_dPlay->setStyleSheet("font-size: 14px;");
    auto* openLoc = new QPushButton(MakeIcon(Icon::Folder, Color::Text, 18), "  Abrir ubicación");
    openLoc->setCursor(::Qt::PointingHandCursor);
    dv->addWidget(m_dCover, 0, ::Qt::AlignHCenter);
    dv->addSpacing(4);
    dv->addWidget(m_dTitle);
    dv->addWidget(m_dAuthor);
    dv->addSpacing(6);
    dv->addWidget(m_dMeta);
    dv->addWidget(m_dPath);
    dv->addStretch();
    dv->addWidget(m_dPlay);
    dv->addWidget(openLoc);
    root->addWidget(m_details);
    m_details->hide();

    connect(m_dPlay, &QPushButton::clicked, this, [this] { if (!m_selectedPath.isEmpty()) emit PlayRequested(m_selectedPath); });
    connect(openLoc, &QPushButton::clicked, this, [this] {
        if (m_selectedPath.isEmpty()) return;
        QFileInfo fi(m_selectedPath);
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath()));
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& idx) {
        ShowDetails(idx.isValid() ? m_model->Game(m_proxy->mapToSource(idx).row()) : nullptr);
    });
    connect(m_view, &QListView::activated, this, [this](const QModelIndex& idx) {
        if (const auto* g = m_model->Game(m_proxy->mapToSource(idx).row())) emit PlayRequested(QString::fromUtf8(g->entry.path.c_str()));
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex idx = m_view->indexAt(pos);
        const auto* g = idx.isValid() ? m_model->Game(m_proxy->mapToSource(idx).row()) : nullptr;
        if (!g) return;
        const QString path = QString::fromUtf8(g->entry.path.c_str());
        QMenu menu(this);
        menu.addAction(MakeIcon(Icon::Play, Color::Text, 16), "Jugar", this, [this, path] { emit PlayRequested(path); });
        menu.addAction(MakeIcon(Icon::Folder, Color::Text, 16), "Abrir ubicación", this, [path] {
            QFileInfo fi(path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath()));
        });
        menu.exec(m_view->viewport()->mapToGlobal(pos));
    });
    UpdateCount();
}

void LibraryPage::SetGames(std::vector<Frontend::GameEntry> games) {
    std::sort(games.begin(), games.end(), [](const auto& a, const auto& b) {
        return QString::fromUtf8(a.title.c_str()).compare(QString::fromUtf8(b.title.c_str()), ::Qt::CaseInsensitive) < 0;
    });
    std::vector<LibraryGame> list;
    list.reserve(games.size());
    for (auto& e : games) {
        LibraryGame g;
        if (!e.icon.empty() && e.icon_w > 0 && e.icon_h > 0)
            g.cover = QImage(e.icon.data(), e.icon_w, e.icon_h, e.icon_w * 4, QImage::Format_RGBA8888).copy();
        else
            g.cover = FallbackCover(QString::fromUtf8(e.title.c_str()), 256);
        g.format = FormatOf(e);
        g.entry = std::move(e);
        g.entry.icon.clear();   // la copia en QImage basta
        list.push_back(std::move(g));
    }
    m_model->SetGames(std::move(list));
    ShowDetails(nullptr);
    UpdateCount();
}

void LibraryPage::FocusSearch() { m_search->setFocus(); m_search->selectAll(); }

bool LibraryPage::eventFilter(QObject* obj, QEvent* e) {
    if (obj == m_view->viewport() && e->type() == QEvent::Resize) {
        const int w = m_view->viewport()->width() - 2;
        const int cell = CardDelegate::CARD_W + 2 * CardDelegate::GAP;
        const int cols = std::max(1, w / cell);
        const QSize grid(std::max(cell, w / cols), CardDelegate::CARD_H + 2 * CardDelegate::GAP);
        if (m_view->gridSize() != grid) m_view->setGridSize(grid);
    }
    return QWidget::eventFilter(obj, e);
}

void LibraryPage::SelectFirst() {
    if (m_proxy->rowCount() > 0) m_view->setCurrentIndex(m_proxy->index(0, 0));
}

void LibraryPage::UpdateCount() {
    const int total = m_model->rowCount(), shown = m_proxy->rowCount();
    if (total == 0) m_count->setText("Tus juegos aparecerán aquí");
    else if (shown == total) m_count->setText(QString("%1 %2").arg(total).arg(total == 1 ? "juego" : "juegos"));
    else m_count->setText(QString("%1 de %2 juegos").arg(shown).arg(total));
    m_body->setCurrentIndex(total == 0 ? 1 : 0);
}

void LibraryPage::ShowDetails(const LibraryGame* g) {
    if (!g) { m_details->hide(); m_selectedPath.clear(); return; }
    m_selectedPath = QString::fromUtf8(g->entry.path.c_str());
    m_dCover->setPixmap(MakeHero(g->cover, 286, 300, devicePixelRatioF()));
    m_dTitle->setText(QString::fromUtf8(g->entry.title.c_str()));
    m_dAuthor->setText(g->entry.author.empty() ? QString("Autor desconocido") : QString::fromUtf8(g->entry.author.c_str()));
    QString meta = "<span style='color:#7C5CFF'>●</span>&nbsp; " + g->format;
    if (!g->entry.version.empty()) meta += "&nbsp;&nbsp;&nbsp;Versión " + QString::fromUtf8(g->entry.version.c_str()).toHtmlEscaped();
    if (g->entry.size) meta += "&nbsp;&nbsp;&nbsp;" + HumanSize(g->entry.size);
    m_dMeta->setText(meta);
    m_dPath->setText(m_selectedPath);
    m_details->show();
}

} // namespace NeXo2::QtUi
