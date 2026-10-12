#pragma once
// Pagina "Biblioteca": cuadricula de tarjetas con la caratula de cada juego, buscador y un
// panel a la derecha con los detalles del elegido y el boton Jugar.
#include <QAbstractListModel>
#include <QImage>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QWidget>
#include <vector>
#include "frontend/game_list.hpp"

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;

namespace NeXo2::QtUi {

struct LibraryGame {
    Frontend::GameEntry entry;
    QImage cover;       // icono del juego (vacio si no tiene)
    QString format;     // "NSP", "XCI", "NRO", "Carpeta"
    mutable QPixmap card;   // caratula ya escalada y redondeada para la tarjeta (se hace una vez)
};

class LibraryModel : public QAbstractListModel {
public:
    enum Role { GameRole = ::Qt::UserRole + 1, SearchRole };
    explicit LibraryModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    void SetGames(std::vector<LibraryGame> games);
    const LibraryGame* Game(int row) const { return row >= 0 && row < int(m_games.size()) ? &m_games[size_t(row)] : nullptr; }
    int rowCount(const QModelIndex& = {}) const override { return int(m_games.size()); }
    QVariant data(const QModelIndex& index, int role) const override;
private:
    std::vector<LibraryGame> m_games;
};

// Dibuja cada tarjeta: caratula redondeada, etiqueta del formato, titulo y autor
class CardDelegate : public QStyledItemDelegate {
public:
    explicit CardDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {CARD_W + 2 * GAP, CARD_H + 2 * GAP}; }
    static constexpr int CARD_W = 188, CARD_H = 262, GAP = 14;
};

class LibraryPage : public QWidget {
    Q_OBJECT
public:
    explicit LibraryPage(QWidget* parent = nullptr);
    void SetGames(std::vector<Frontend::GameEntry> games);
    void FocusSearch();
    void SelectFirst();
signals:
    void PlayRequested(const QString& path);
    void AddFolderRequested();
    void OpenFileRequested();
    void RefreshRequested();
protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
private:
    void ShowDetails(const LibraryGame* g);
    void UpdateCount();
    LibraryModel* m_model;
    QSortFilterProxyModel* m_proxy;
    QListView* m_view;
    QLineEdit* m_search;
    QLabel* m_count;
    QStackedWidget* m_body;      // la cuadricula o el mensaje de "no hay juegos"
    // Panel de detalles
    QWidget* m_details;
    QStackedWidget* m_detailsStack;   // 0 = nada elegido, 1 = el juego
    QSize m_lastViewport;
    QLabel* m_dCover;
    QLabel* m_dTitle;
    QLabel* m_dAuthor;
    QLabel* m_dMeta;
    QLabel* m_dPath;
    QPushButton* m_dPlay;
    QString m_selectedPath;
};

} // namespace NeXo2::QtUi
