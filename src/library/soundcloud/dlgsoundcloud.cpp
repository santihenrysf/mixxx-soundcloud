#include "library/soundcloud/dlgsoundcloud.h"

#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDrag>
#include <QPointer>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QToolButton>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScroller>
#include <QSet>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "moc_dlgsoundcloud.cpp"
#include "track/track.h"
#include "track/trackref.h"
#include "util/dnd.h"
#include "widget/wlibrary.h"

namespace {

const QString kConfigGroup = QStringLiteral("[SoundCloud]");
const ConfigKey kBridgeDirKey(kConfigGroup, QStringLiteral("BridgeDir"));
const ConfigKey kDownloadDirKey(kConfigGroup, QStringLiteral("DownloadDir"));
const ConfigKey kProfileKey(kConfigGroup, QStringLiteral("Profile"));
const ConfigKey kProfileNameKey(kConfigGroup, QStringLiteral("ProfileName"));
const ConfigKey kViewModeKey(kConfigGroup, QStringLiteral("ViewMode"));

// Datos guardados en la celda del tema
constexpr int kUrlRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;
constexpr int kArtistRole = Qt::UserRole + 3;
constexpr int kStateRole = Qt::UserRole + 4;
constexpr int kProgressRole = Qt::UserRole + 5;
constexpr int kPreviewRole = Qt::UserRole + 6;

constexpr int kDecksRole = Qt::UserRole + 7;  // decks que tienen el tema cargado (bit 0 = A)
constexpr int kTargetRole = Qt::UserRole + 8; // decks a los que se esta cargando
constexpr int kBigCoverRole = Qt::UserRole + 9; // portada grande para la grilla

constexpr int kColLoad = 0;
constexpr int kColTrack = 1;
constexpr int kColDuration = 2;
constexpr int kColAction = 3;

const QColor kDeckColors[2] = {QColor(0x3f, 0xb6, 0xf2), QColor(0xff, 0x8a, 0x3d)};
const QString kDeckGroups[2] = {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")};

int deckBit(const QString& group) {
    for (int i = 0; i < 2; ++i) {
        if (group == kDeckGroups[i]) {
            return 1 << i;
        }
    }
    return 0;
}

// Botones A / B dentro de la celda LOAD
QRect deckChipRect(const QRect& cell, int deck) {
    const int size = 24;
    return QRect(cell.left() + 10 + deck * (size + 6), cell.center().y() - size / 2 + 1, size, size);
}

int deckChipAt(const QRect& cell, const QPoint& pos) {
    for (int i = 0; i < 2; ++i) {
        if (deckChipRect(cell, i).adjusted(-3, -6, 3, 6).contains(pos)) {
            return i;
        }
    }
    return -1;
}

bool samePath(const QString& a, const QString& b) {
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    const QString ca = QFileInfo(a).canonicalFilePath();
    const QString cb = QFileInfo(b).canonicalFilePath();
#ifdef Q_OS_WIN
    return !ca.isEmpty() && ca.compare(cb, Qt::CaseInsensitive) == 0;
#else
    return !ca.isEmpty() && ca == cb;
#endif
}

// Estados de descarga
constexpr int kStateNone = 0;
constexpr int kStateQueued = 1;
constexpr int kStateDownloading = 2;
constexpr int kStateProcessing = 3;
constexpr int kStateDone = 4;
constexpr int kStateError = 5;

// Paleta (oscura, en linea con los skins de Mixxx)
const QColor kText(0xec, 0xec, 0xee);
const QColor kMuted(0x8a, 0x8a, 0x93);
const QColor kSurface(0x26, 0x26, 0x2a);
const QColor kAccent(0xff, 0x6a, 0x1a);
const QColor kOk(0x3e, 0xcf, 0x8e);
const QColor kErr(0xff, 0x5d, 0x5d);

const QString kStyleSheet = QStringLiteral(R"(
#SoundCloudRoot { background: #151517; }
#scNav {
  background: #1c1c1f; border: none; border-radius: 12px; padding: 8px;
  color: #c9c9cf; font-size: 13px; outline: 0;
}
#scNav::item { padding: 9px 10px; border-radius: 8px; margin: 1px 0; }
#scNav::item:selected { background: #2e2e33; color: #ffffff; }
#scNav::item:hover:!selected { background: #232327; }
#scPanel { background: #1c1c1f; border-radius: 12px; }
#scTitle { color: #ffffff; font-size: 19px; font-weight: 600; }
#scSubtitle { color: #8a8a93; font-size: 12px; }
#scSearch, #scCombo {
  background: #26262a; border: 1px solid #2f2f34; border-radius: 8px;
  padding: 7px 10px; color: #ececee; font-size: 13px;
  selection-background-color: #ff6a1a;
}
#scSearch:focus, #scCombo:focus { border-color: #ff6a1a; }
#scCombo QAbstractItemView {
  background: #1c1c1f; color: #ececee; border: 1px solid #2f2f34;
  selection-background-color: #2e2e33; outline: 0;
}
#scTable {
  background: transparent; border: none; color: #ececee; outline: 0;
  selection-background-color: #2a2a2f; alternate-background-color: transparent;
}
#scTable::item { border: none; padding: 0; }
#scTable::item:hover { background: #232327; }
#scTable::item:selected { background: #2a2a2f; }
QPushButton[scButton="true"] {
  background: #26262a; color: #dcdce0; border: 1px solid #2f2f34;
  border-radius: 8px; padding: 6px 12px; font-size: 12px;
}
QPushButton[scButton="true"]:hover { background: #2e2e33; color: #ffffff; }
QPushButton[scButton="true"]:pressed { background: #34343a; }
QPushButton[scButton="true"]:disabled { color: #55555c; }
QPushButton[scPrimary="true"] { background: #ff6a1a; color: #ffffff; border: none; }
QPushButton[scPrimary="true"]:hover { background: #ff7d36; }
QPushButton[scLink="true"] {
  background: transparent; border: none; color: #8a8a93; font-size: 12px;
  padding: 4px 6px; text-align: left;
}
QPushButton[scLink="true"]:hover { color: #ffffff; }
#scStatus { color: #8a8a93; font-size: 12px; }
#scGrid { background: transparent; border: none; outline: 0; }
QToolButton[scView="true"] {
  background: #26262a; color: #8a8a93; border: 1px solid #2f2f34; border-radius: 8px;
  min-width: 32px; max-width: 32px; min-height: 28px; max-height: 28px; font-size: 15px;
}
QToolButton[scView="true"]:hover { color: #ffffff; }
QToolButton[scView="true"]:checked { background: #ff6a1a; border-color: #ff6a1a; color: #ffffff; }
#scCard { background: #232327; border: 1px solid #2f2f34; border-radius: 14px; }
#scCardTitle { color: #ffffff; font-size: 17px; font-weight: 600; }
#scCardText { color: #8a8a93; font-size: 12px; }
#scProfileEdit {
  background: #1a1a1d; border: 1px solid #34343a; border-radius: 9px;
  padding: 9px 12px; color: #ececee; font-size: 13px; selection-background-color: #ff6a1a;
}
#scProfileEdit:focus { border-color: #ff6a1a; }
#scProfileEdit[invalid="true"] { border-color: #ff5d5d; }
#scProfileError { color: #ff5d5d; font-size: 12px; }
#scCheck {
  color: #151517; background: #3ecf8e; border-radius: 22px; font-size: 22px; font-weight: 700;
  min-width: 44px; max-width: 44px; min-height: 44px; max-height: 44px;
}
#scCardDoneText { color: #ffffff; font-size: 15px; font-weight: 600; }
QPushButton[scChip="true"] {
  background: #1c1c1f; color: #dcdce0; border: 1px solid #2f2f34; border-radius: 10px;
  padding: 8px 10px; font-size: 12px; text-align: left;
}
QPushButton[scChip="true"]:hover { border-color: #ff6a1a; color: #ffffff; }
#scProgress { background: #26262a; border: none; border-radius: 2px; max-height: 4px; }
#scProgress::chunk { background: #ff6a1a; border-radius: 2px; }
#SoundCloudRoot QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
#SoundCloudRoot QScrollBar::handle:vertical { background: #3a3a40; border-radius: 3px; min-height: 30px; }
#SoundCloudRoot QScrollBar::add-line:vertical, #SoundCloudRoot QScrollBar::sub-line:vertical { height: 0; }
#SoundCloudRoot QScrollBar::add-page:vertical, #SoundCloudRoot QScrollBar::sub-page:vertical { background: none; }
)");

QString formatDuration(qint64 ms) {
    const qint64 totalSeconds = ms / 1000;
    return QStringLiteral("%1:%2")
            .arg(totalSeconds / 60)
            .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

ConfigKey downloadedKey(const QString& url) {
    // Las claves de configuracion no pueden tener espacios ni '='
    QString key = url;
    key.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9/_.-]")), QStringLiteral("_"));
    return ConfigKey(kConfigGroup, QStringLiteral("dl_") + key);
}

QPixmap roundedPixmap(const QPixmap& source, int size, int radius) {
    QPixmap scaled = source.scaled(size * 2,
            size * 2,
            Qt::KeepAspectRatioByExpanding,
            Qt::SmoothTransformation);
    QPixmap result(size * 2, size * 2);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(0, 0, size * 2, size * 2, radius * 2, radius * 2);
    painter.setClipPath(path);
    painter.drawPixmap((size * 2 - scaled.width()) / 2, (size * 2 - scaled.height()) / 2, scaled);
    painter.end();
    result.setDevicePixelRatio(2.0);
    return result;
}

void prepareButton(QPushButton* pButton, const char* role = "scButton") {
    pButton->setProperty(role, true);
    if (qstrcmp(role, "scButton") != 0) {
        pButton->setProperty("scButton", true);
    }
    pButton->setCursor(Qt::PointingHandCursor);
}

/// Pinta la celda del tema: portada redondeada + titulo + artista.
class TrackDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        const QWidget* pWidget = opt.widget;
        QStyle* pStyle = pWidget ? pWidget->style() : nullptr;
        if (pStyle) {
            pStyle->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, pWidget);
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect;
        const int cover = 38;
        const QRect coverRect(r.left() + 10, r.top() + (r.height() - cover) / 2, cover, cover);
        const QPixmap pixmap = index.data(Qt::DecorationRole).value<QPixmap>();
        if (!pixmap.isNull()) {
            painter->drawPixmap(coverRect, pixmap);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kSurface);
            painter->drawRoundedRect(coverRect, 6, 6);
            painter->setPen(kMuted);
            painter->drawText(coverRect, Qt::AlignCenter, QString(QChar(0x266A)));
        }

        const int textLeft = coverRect.right() + 12;
        const QRect textRect(textLeft, r.top(), r.right() - textLeft - 8, r.height());
        QFont titleFont = opt.font;
        titleFont.setPointSizeF(titleFont.pointSizeF() > 0 ? titleFont.pointSizeF() + 0.5 : 10.5);
        titleFont.setWeight(QFont::DemiBold);
        QFont artistFont = opt.font;
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics artistMetrics(artistFont);
        const int block = titleMetrics.height() + artistMetrics.height() + 2;
        const int top = r.top() + (r.height() - block) / 2;

        painter->setFont(titleFont);
        painter->setPen(kText);
        painter->drawText(QRect(textRect.left(), top, textRect.width(), titleMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                        Qt::ElideRight,
                        textRect.width()));

        QString artist = index.data(kArtistRole).toString();
        if (index.data(kPreviewRole).toBool()) {
            artist += QStringLiteral("  ·  solo 30 s");
        }
        painter->setFont(artistFont);
        painter->setPen(kMuted);
        painter->drawText(QRect(textRect.left(),
                                  top + titleMetrics.height() + 2,
                                  textRect.width(),
                                  artistMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                artistMetrics.elidedText(artist, Qt::ElideRight, textRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(56);
        return size;
    }
};

/// Pinta la celda de accion: flecha de descarga, progreso, listo o error.
class ActionDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget* pWidget = opt.widget;
        if (pWidget) {
            pWidget->style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, pWidget);
        }

        const QModelIndex trackIndex = index.sibling(index.row(), kColTrack);
        const int state = trackIndex.data(kStateRole).toInt();
        const int progress = trackIndex.data(kProgressRole).toInt();
        const QRect r = option.rect.adjusted(10, 0, -12, 0);
        const bool hovered = option.state & QStyle::State_MouseOver;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QFont font = opt.font;
        painter->setFont(font);
        switch (state) {
        case kStateDownloading:
        case kStateProcessing: {
            const int barWidth = std::min(90, r.width() - 40);
            const QRect bar(r.right() - barWidth, r.center().y() - 2, barWidth, 4);
            painter->setPen(Qt::NoPen);
            painter->setBrush(kSurface);
            painter->drawRoundedRect(bar, 2, 2);
            const int pct = state == kStateProcessing ? 100 : progress;
            painter->setBrush(kAccent);
            painter->drawRoundedRect(QRect(bar.left(), bar.top(), bar.width() * pct / 100, 4), 2, 2);
            painter->setPen(kAccent);
            painter->drawText(QRect(r.left(), r.top(), bar.left() - r.left() - 8, r.height()),
                    Qt::AlignRight | Qt::AlignVCenter,
                    state == kStateProcessing ? QStringLiteral("…") : QStringLiteral("%1%").arg(pct));
            break;
        }
        case kStateQueued:
            painter->setPen(kMuted);
            painter->drawText(r, Qt::AlignRight | Qt::AlignVCenter, QObject::tr("En cola"));
            break;
        case kStateDone:
            painter->setPen(kOk);
            painter->drawText(r, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("✓  ") + QObject::tr("Listo"));
            break;
        case kStateError:
            painter->setPen(kErr);
            painter->drawText(r, Qt::AlignRight | Qt::AlignVCenter, QObject::tr("Error · reintentar"));
            break;
        default: {
            // Boton circular de descarga
            const int d = 26;
            const QRect circle(r.right() - d, r.center().y() - d / 2, d, d);
            painter->setPen(QPen(hovered ? kAccent : QColor(0x3a, 0x3a, 0x40), 1.2));
            painter->setBrush(hovered ? QColor(0xff, 0x6a, 0x1a, 40) : Qt::transparent);
            painter->drawEllipse(circle);
            QPen arrow(hovered ? kAccent : kMuted, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter->setPen(arrow);
            const QPointF c = circle.center();
            painter->drawLine(QPointF(c.x(), c.y() - 5.5), QPointF(c.x(), c.y() + 4.5));
            painter->drawLine(QPointF(c.x() - 4, c.y() + 0.5), QPointF(c.x(), c.y() + 4.5));
            painter->drawLine(QPointF(c.x() + 4, c.y() + 0.5), QPointF(c.x(), c.y() + 4.5));
            break;
        }
        }
        painter->restore();
    }
};

/// Pinta la columna LOAD: botones A y B. Lleno = cargado en ese deck,
/// borde grueso = cargandose, contorno = disponible.
class LoadDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget* pWidget = opt.widget;
        if (pWidget) {
            pWidget->style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, pWidget);
        }
        const QModelIndex trackIndex = index.sibling(index.row(), kColTrack);
        const int loaded = trackIndex.data(kDecksRole).toInt();
        const int target = trackIndex.data(kTargetRole).toInt();
        QPoint mouse(-1, -1);
        if (const auto* pView = qobject_cast<const QAbstractItemView*>(pWidget)) {
            mouse = pView->viewport()->mapFromGlobal(QCursor::pos());
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QFont font = opt.font;
        font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        for (int i = 0; i < 2; ++i) {
            const QRectF chip = QRectF(deckChipRect(option.rect, i)).adjusted(0.5, 0.5, -0.5, -0.5);
            const QColor color = kDeckColors[i];
            const bool isLoaded = loaded & (1 << i);
            const bool isTarget = target & (1 << i);
            const bool hover = (option.state & QStyle::State_MouseOver) && chip.contains(mouse);
            QColor fill = Qt::transparent;
            if (isLoaded) {
                fill = color;
            } else if (hover || isTarget) {
                fill = QColor(color.red(), color.green(), color.blue(), 45);
            }
            QColor border = color;
            if (!isLoaded && !isTarget && !hover) {
                border.setAlpha(150);
            }
            painter->setPen(QPen(border, isTarget ? 2.0 : 1.2));
            painter->setBrush(fill);
            painter->drawRoundedRect(chip, 6, 6);
            painter->setPen(isLoaded ? QColor(0x15, 0x15, 0x17) : color);
            painter->drawText(chip, Qt::AlignCenter, QString(QChar('A' + i)));
        }
        painter->restore();
    }
};

/// Tarjeta que acompana al mouse mientras se arrastra un tema.
QPixmap dragCardPixmap(const QString& title,
        const QString& artist,
        const QPixmap& cover,
        bool downloaded,
        const QFont& baseFont) {
    const int w = 300;
    const int h = 60;
    QPixmap pixmap(w * 2, h * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kAccent, 1.5));
    p.setBrush(QColor(0x23, 0x23, 0x27, 240));
    p.drawRoundedRect(QRectF(1, 1, w - 2, h - 2), 12, 12);
    const QRect coverRect(10, 10, 40, 40);
    if (!cover.isNull()) {
        p.drawPixmap(coverRect, cover);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(kSurface);
        p.drawRoundedRect(coverRect, 6, 6);
    }
    QFont titleFont = baseFont;
    titleFont.setWeight(QFont::DemiBold);
    const QFontMetrics fm(titleFont);
    p.setFont(titleFont);
    p.setPen(kText);
    p.drawText(QRect(60, 10, w - 70, 20), Qt::AlignLeft | Qt::AlignVCenter,
            fm.elidedText(title, Qt::ElideRight, w - 70));
    p.setFont(baseFont);
    p.setPen(kAccent);
    const QString hint = downloaded
            ? QObject::tr("Soltalo en un deck")
            : QObject::tr("Soltalo en un deck · se descarga y carga");
    p.drawText(QRect(60, 30, w - 70, 20), Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(baseFont).elidedText(artist.isEmpty() ? hint : artist + QStringLiteral("  ·  ") + hint,
                    Qt::ElideRight,
                    w - 70));
    p.end();
    return pixmap;
}

/// Tabla que permite arrastrar temas a los decks, descargados o no.
class SoundCloudTable : public QTableWidget {
  public:
    using QTableWidget::QTableWidget;

  protected:
    QMimeData* mimeData(const QList<QTableWidgetItem*>& items) const override {
        // Un deck carga un solo tema: se arrastra el primero de la seleccion
        const QTableWidgetItem* pFirst = nullptr;
        for (const QTableWidgetItem* pItem : items) {
            if (pItem && (!pFirst || pItem->row() < pFirst->row())) {
                pFirst = pItem;
            }
        }
        const QTableWidgetItem* pTrack = pFirst ? item(pFirst->row(), kColTrack) : nullptr;
        if (!pTrack) {
            return nullptr;
        }
        auto* pMimeData = new QMimeData();
        const QString path = pTrack->data(kPathRole).toString();
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            pMimeData->setUrls({QUrl::fromLocalFile(path)});
        } else {
            pMimeData->setData(DragAndDropHelper::kRemoteTrackMimeType,
                    pTrack->data(kUrlRole).toString().toUtf8());
        }
        return pMimeData;
    }

    void startDrag(Qt::DropActions supportedActions) override {
        Q_UNUSED(supportedActions);
        const int row = currentRow();
        const QTableWidgetItem* pTrack = row >= 0 ? item(row, kColTrack) : nullptr;
        QMimeData* pMimeData = pTrack ? mimeData({const_cast<QTableWidgetItem*>(pTrack)}) : nullptr;
        if (!pMimeData) {
            return;
        }
        auto* pDrag = new QDrag(this);
        pDrag->setMimeData(pMimeData);
        pDrag->setPixmap(dragCardPixmap(pTrack->text(),
                pTrack->data(kArtistRole).toString(),
                pTrack->data(Qt::DecorationRole).value<QPixmap>(),
                pMimeData->hasUrls(),
                font()));
        pDrag->setHotSpot(QPoint(24, 30));
        pDrag->exec(Qt::CopyAction);
    }
};

// ---- Grilla estilo SoundCloud
constexpr int kGridCell = 186;   // ancho de cada tarjeta
constexpr int kGridCover = 170;  // lado de la portada
constexpr int kGridHeight = 238;

QRect gridCoverRect(const QRect& cell) {
    return QRect(cell.left() + (cell.width() - kGridCover) / 2, cell.top() + 6, kGridCover, kGridCover);
}
// A/B abajo a la izquierda de la portada, descarga abajo a la derecha
QRect gridChipRect(const QRect& cell, int deck) {
    const QRect cover = gridCoverRect(cell);
    return QRect(cover.left() + 8 + deck * 30, cover.bottom() - 32, 26, 26);
}
QRect gridActionRect(const QRect& cell) {
    const QRect cover = gridCoverRect(cell);
    return QRect(cover.right() - 34, cover.bottom() - 34, 28, 28);
}

class GridDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(kGridCell, kGridHeight);
    }

    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect cell = option.rect;
        const QRect cover = gridCoverRect(cell);
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool selected = option.state & QStyle::State_Selected;
        QPoint mouse(-1, -1);
        if (const auto* pView = qobject_cast<const QAbstractItemView*>(option.widget)) {
            mouse = pView->viewport()->mapFromGlobal(QCursor::pos());
        }

        // Portada
        QPainterPath clip;
        clip.addRoundedRect(QRectF(cover), 8, 8);
        painter->setClipPath(clip);
        QPixmap big = index.data(kBigCoverRole).value<QPixmap>();
        if (big.isNull()) {
            big = index.data(Qt::DecorationRole).value<QPixmap>();
        }
        if (!big.isNull()) {
            painter->drawPixmap(cover, big);
        } else {
            painter->fillRect(cover, kSurface);
            painter->setPen(kMuted);
            QFont note = option.font;
            note.setPointSizeF(note.pointSizeF() > 0 ? note.pointSizeF() * 3 : 30);
            painter->setFont(note);
            painter->drawText(cover, Qt::AlignCenter, QString(QChar(0x266A)));
        }
        // degradado inferior para que se lean los botones
        QLinearGradient shade(cover.bottomLeft() - QPoint(0, 56), cover.bottomLeft());
        shade.setColorAt(0, QColor(0, 0, 0, 0));
        shade.setColorAt(1, QColor(0, 0, 0, hovered ? 170 : 120));
        painter->fillRect(QRect(cover.left(), cover.bottom() - 56, cover.width(), 57), shade);
        painter->setClipping(false);
        if (selected || hovered) {
            painter->setPen(QPen(selected ? kAccent : QColor(0x4a, 0x4a, 0x52), 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(cover).adjusted(1, 1, -1, -1), 8, 8);
        }

        // Botones A / B
        const int loaded = index.data(kDecksRole).toInt();
        const int target = index.data(kTargetRole).toInt();
        QFont chipFont = option.font;
        chipFont.setWeight(QFont::DemiBold);
        painter->setFont(chipFont);
        for (int i = 0; i < 2; ++i) {
            const QRectF chip = QRectF(gridChipRect(cell, i)).adjusted(0.5, 0.5, -0.5, -0.5);
            const QColor color = kDeckColors[i];
            const bool isLoaded = loaded & (1 << i);
            const bool isTarget = target & (1 << i);
            const bool over = chip.contains(mouse);
            if (!isLoaded && !isTarget && !hovered) {
                continue; // limpio: solo aparecen al pasar el mouse o si estan en uso
            }
            painter->setPen(QPen(color, isTarget ? 2.0 : 1.2));
            painter->setBrush(isLoaded ? color : QColor(0x15, 0x15, 0x17, over ? 255 : 210));
            painter->drawRoundedRect(chip, 7, 7);
            painter->setPen(isLoaded ? QColor(0x15, 0x15, 0x17) : color);
            painter->drawText(chip, Qt::AlignCenter, QString(QChar('A' + i)));
        }

        // Estado de descarga
        const int state = index.data(kStateRole).toInt();
        const int progress = index.data(kProgressRole).toInt();
        const QRect action = gridActionRect(cell);
        if (state == kStateDownloading || state == kStateProcessing) {
            const QRect bar(cover.left(), cover.bottom() - 3, cover.width(), 4);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, 160));
            painter->drawRect(bar);
            painter->setBrush(kAccent);
            painter->drawRect(QRect(bar.left(), bar.top(), bar.width() * (state == kStateProcessing ? 100 : progress) / 100, 4));
            painter->setPen(Qt::white);
            painter->drawText(action.adjusted(-20, 0, 0, 0), Qt::AlignRight | Qt::AlignVCenter,
                    state == kStateProcessing ? QStringLiteral("…") : QStringLiteral("%1%").arg(progress));
        } else if (state == kStateDone) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kOk);
            painter->drawEllipse(action.adjusted(4, 4, -4, -4));
            painter->setPen(QPen(QColor(0x15, 0x15, 0x17), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF c = QRectF(action).center();
            painter->drawLine(QPointF(c.x() - 4.5, c.y() + 0.5), QPointF(c.x() - 1.5, c.y() + 3.5));
            painter->drawLine(QPointF(c.x() - 1.5, c.y() + 3.5), QPointF(c.x() + 4.5, c.y() - 3));
        } else if (state == kStateQueued) {
            painter->setPen(Qt::white);
            painter->drawText(action.adjusted(-40, 0, 0, 0), Qt::AlignRight | Qt::AlignVCenter, QObject::tr("En cola"));
        } else if (state == kStateError) {
            painter->setPen(kErr);
            painter->drawText(action.adjusted(-40, 0, 0, 0), Qt::AlignRight | Qt::AlignVCenter, QObject::tr("Error"));
        } else if (hovered) {
            const bool over = action.contains(mouse);
            painter->setPen(QPen(over ? kAccent : QColor(255, 255, 255, 200), 1.4));
            painter->setBrush(QColor(0x15, 0x15, 0x17, 200));
            painter->drawEllipse(action.adjusted(1, 1, -1, -1));
            painter->setPen(QPen(over ? kAccent : Qt::white, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF c = QRectF(action).center();
            painter->drawLine(QPointF(c.x(), c.y() - 6), QPointF(c.x(), c.y() + 5));
            painter->drawLine(QPointF(c.x() - 4.5, c.y() + 0.5), QPointF(c.x(), c.y() + 5));
            painter->drawLine(QPointF(c.x() + 4.5, c.y() + 0.5), QPointF(c.x(), c.y() + 5));
        }

        // Titulo y artista
        QFont titleFont = option.font;
        titleFont.setWeight(QFont::DemiBold);
        const QFontMetrics tm(titleFont);
        const QFontMetrics am(option.font);
        const QRect titleRect(cover.left(), cover.bottom() + 8, cover.width(), tm.height());
        painter->setFont(titleFont);
        painter->setPen(kText);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                tm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, cover.width()));
        QString artist = index.data(kArtistRole).toString();
        if (index.data(kPreviewRole).toBool()) {
            artist += QStringLiteral("  ·  30 s");
        }
        painter->setFont(option.font);
        painter->setPen(kMuted);
        painter->drawText(QRect(cover.left(), titleRect.bottom() + 2, cover.width(), am.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                am.elidedText(artist, Qt::ElideRight, cover.width()));
        painter->restore();
    }
};

class SoundCloudGrid : public QListView {
  public:
    using QListView::QListView;

  protected:
    void startDrag(Qt::DropActions supportedActions) override {
        Q_UNUSED(supportedActions);
        const QModelIndex index = currentIndex();
        if (!index.isValid()) {
            return;
        }
        QMimeData* pMimeData = model()->mimeData({index});
        if (!pMimeData) {
            return;
        }
        auto* pDrag = new QDrag(this);
        pDrag->setMimeData(pMimeData);
        pDrag->setPixmap(dragCardPixmap(index.data(Qt::DisplayRole).toString(),
                index.data(kArtistRole).toString(),
                index.data(Qt::DecorationRole).value<QPixmap>(),
                pMimeData->hasUrls(),
                font()));
        pDrag->setHotSpot(QPoint(24, 30));
        pDrag->exec(Qt::CopyAction);
    }
    void mouseMoveEvent(QMouseEvent* pEvent) override {
        QListView::mouseMoveEvent(pEvent);
        viewport()->update(); // hover de los botones A/B
    }
};

} // anonymous namespace

DlgSoundCloud::DlgSoundCloud(WLibrary* parent,
        UserSettingsPointer pConfig,
        Library* pLibrary)
        : QWidget(parent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_page(Page::Search),
          m_pListProcess(nullptr),
          m_listKind(ListKind::Tracks),
          m_pDownloadProcess(nullptr),
          m_pNetwork(new QNetworkAccessManager(this)),
          m_currentAfter(After::Nothing) {
    m_pendingPage = Page::Search;
    m_gridMode = true;
    setObjectName(QStringLiteral("SoundCloudRoot"));
    setAttribute(Qt::WA_StyledBackground, true);
    applyStyle();
    connect(&m_styleWatcher,
            &QFileSystemWatcher::fileChanged,
            this,
            &DlgSoundCloud::slotStyleFileChanged);

    auto* pRoot = new QHBoxLayout(this);
    pRoot->setContentsMargins(8, 8, 8, 8);
    pRoot->setSpacing(8);

    // ---- Menu lateral
    auto* pSide = new QVBoxLayout();
    pSide->setSpacing(6);
    m_pNav = new QListWidget(this);
    m_pNav->setObjectName(QStringLiteral("scNav"));
    m_pNav->setFixedWidth(176);
    m_pNav->setFrameShape(QFrame::NoFrame);
    m_pNav->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pNav->addItem(QStringLiteral("⌕   ") + tr("Buscar"));
    m_pNav->addItem(QStringLiteral("♡   ") + tr("Mis likes"));
    m_pNav->addItem(QStringLiteral("≡   ") + tr("Mis playlists"));
    m_pNav->addItem(QStringLiteral("↓   ") + tr("Descargas"));
    m_pNav->setCurrentRow(0);
    pSide->addWidget(m_pNav, 1);
    m_pProfileButton = new QPushButton(this);
    m_pProfileButton->setProperty("scChip", true);
    m_pProfileButton->setCursor(Qt::PointingHandCursor);
    m_pProfileButton->setFixedWidth(176);
    m_pFolderButton = new QPushButton(tr("Carpeta de descargas"), this);
    prepareButton(m_pFolderButton, "scLink");
    m_pOpenFolderButton = new QPushButton(tr("Abrir carpeta"), this);
    prepareButton(m_pOpenFolderButton, "scLink");
    pSide->addWidget(m_pProfileButton);
    pSide->addWidget(m_pFolderButton);
    pSide->addWidget(m_pOpenFolderButton);
    pRoot->addLayout(pSide);

    // ---- Panel principal
    auto* pPanel = new QWidget(this);
    pPanel->setObjectName(QStringLiteral("scPanel"));
    pPanel->setAttribute(Qt::WA_StyledBackground, true);
    auto* pMain = new QVBoxLayout(pPanel);
    pMain->setContentsMargins(16, 14, 16, 10);
    pMain->setSpacing(8);

    auto* pHeader = new QHBoxLayout();
    auto* pHeaderText = new QVBoxLayout();
    pHeaderText->setSpacing(0);
    m_pTitle = new QLabel(pPanel);
    m_pTitle->setObjectName(QStringLiteral("scTitle"));
    m_pSubtitle = new QLabel(pPanel);
    m_pSubtitle->setObjectName(QStringLiteral("scSubtitle"));
    pHeaderText->addWidget(m_pTitle);
    pHeaderText->addWidget(m_pSubtitle);
    pHeader->addLayout(pHeaderText, 1);
    m_pDeck1Button = new QPushButton(tr("Deck 1"), pPanel);
    m_pDeck2Button = new QPushButton(tr("Deck 2"), pPanel);
    m_pDownloadButton = new QPushButton(tr("Descargar"), pPanel);
    m_pDownloadAllButton = new QPushButton(tr("Descargar todo"), pPanel);
    prepareButton(m_pDeck1Button);
    prepareButton(m_pDeck2Button);
    prepareButton(m_pDownloadButton);
    prepareButton(m_pDownloadAllButton, "scPrimary");
    m_pDeck1Button->setToolTip(tr("Descarga (si hace falta) y carga el tema elegido en el Deck 1"));
    m_pDeck2Button->setToolTip(tr("Descarga (si hace falta) y carga el tema elegido en el Deck 2"));
    m_pDownloadButton->setToolTip(tr("Descarga los temas seleccionados"));
    m_pDownloadAllButton->setToolTip(tr("Descarga toda la lista"));
    m_pGridButton = new QToolButton(pPanel);
    m_pGridButton->setText(QStringLiteral("▦"));
    m_pGridButton->setToolTip(tr("Vista grilla"));
    m_pListButton = new QToolButton(pPanel);
    m_pListButton->setText(QStringLiteral("☰"));
    m_pListButton->setToolTip(tr("Vista lista"));
    for (QToolButton* pButton : {m_pGridButton, m_pListButton}) {
        pButton->setProperty("scView", true);
        pButton->setCheckable(true);
        pButton->setCursor(Qt::PointingHandCursor);
        pHeader->addWidget(pButton, 0, Qt::AlignBottom);
    }
    pHeader->addSpacing(10);
    pHeader->addWidget(m_pDeck1Button, 0, Qt::AlignBottom);
    pHeader->addWidget(m_pDeck2Button, 0, Qt::AlignBottom);
    pHeader->addWidget(m_pDownloadButton, 0, Qt::AlignBottom);
    pHeader->addWidget(m_pDownloadAllButton, 0, Qt::AlignBottom);
    pMain->addLayout(pHeader);

    m_pSearchEdit = new QLineEdit(pPanel);
    m_pSearchEdit->setObjectName(QStringLiteral("scSearch"));
    m_pSearchEdit->setPlaceholderText(tr("Buscar en SoundCloud: artista, tema, sello…"));
    m_pSearchEdit->setClearButtonEnabled(true);
    pMain->addWidget(m_pSearchEdit);

    m_pPlaylistCombo = new QComboBox(pPanel);
    m_pPlaylistCombo->setObjectName(QStringLiteral("scCombo"));
    m_pPlaylistCombo->setPlaceholderText(tr("Elegí una playlist…"));
    m_pPlaylistCombo->setVisible(false);
    pMain->addWidget(m_pPlaylistCombo);

    // ---- Tarjeta para conectar el perfil de SoundCloud
    m_pProfileCard = new QWidget(pPanel);
    auto* pCardOuter = new QHBoxLayout(m_pProfileCard);
    pCardOuter->setContentsMargins(0, 24, 0, 24);
    auto* pCard = new QWidget(m_pProfileCard);
    pCard->setObjectName(QStringLiteral("scCard"));
    pCard->setAttribute(Qt::WA_StyledBackground, true);
    pCard->setFixedWidth(440);
    auto* pCardLayout = new QVBoxLayout(pCard);
    pCardLayout->setContentsMargins(24, 22, 24, 22);
    pCardLayout->setSpacing(10);

    m_pCardForm = new QWidget(pCard);
    auto* pForm = new QVBoxLayout(m_pCardForm);
    pForm->setContentsMargins(0, 0, 0, 0);
    pForm->setSpacing(10);
    m_pCardTitle = new QLabel(tr("Conectá tu SoundCloud"), m_pCardForm);
    m_pCardTitle->setObjectName(QStringLiteral("scCardTitle"));
    auto* pCardText = new QLabel(tr("Pegá el link de tu perfil o escribí tu usuario. "
                                    "Lo usamos para mostrar tus likes y tus playlists."),
            m_pCardForm);
    pCardText->setObjectName(QStringLiteral("scCardText"));
    pCardText->setWordWrap(true);
    m_pProfileEdit = new QLineEdit(m_pCardForm);
    m_pProfileEdit->setObjectName(QStringLiteral("scProfileEdit"));
    m_pProfileEdit->setPlaceholderText(QStringLiteral("soundcloud.com/tu-usuario"));
    m_pProfileEdit->setClearButtonEnabled(true);
    m_pProfileError = new QLabel(m_pCardForm);
    m_pProfileError->setObjectName(QStringLiteral("scProfileError"));
    m_pProfileError->setWordWrap(true);
    m_pProfileError->setVisible(false);
    auto* pButtons = new QHBoxLayout();
    pButtons->setSpacing(8);
    m_pCancelButton = new QPushButton(tr("Cancelar"), m_pCardForm);
    prepareButton(m_pCancelButton);
    m_pConnectButton = new QPushButton(tr("Conectar"), m_pCardForm);
    prepareButton(m_pConnectButton, "scPrimary");
    m_pConnectButton->setDefault(true);
    pButtons->addStretch(1);
    pButtons->addWidget(m_pCancelButton);
    pButtons->addWidget(m_pConnectButton);
    pForm->addWidget(m_pCardTitle);
    pForm->addWidget(pCardText);
    pForm->addSpacing(4);
    pForm->addWidget(m_pProfileEdit);
    pForm->addWidget(m_pProfileError);
    pForm->addSpacing(4);
    pForm->addLayout(pButtons);

    m_pCardDone = new QWidget(pCard);
    auto* pDone = new QVBoxLayout(m_pCardDone);
    pDone->setContentsMargins(0, 8, 0, 8);
    pDone->setSpacing(10);
    auto* pCheck = new QLabel(QStringLiteral("✓"), m_pCardDone);
    pCheck->setObjectName(QStringLiteral("scCheck"));
    pCheck->setAlignment(Qt::AlignCenter);
    m_pCardDoneText = new QLabel(m_pCardDone);
    m_pCardDoneText->setObjectName(QStringLiteral("scCardDoneText"));
    m_pCardDoneText->setAlignment(Qt::AlignCenter);
    pDone->addWidget(pCheck, 0, Qt::AlignHCenter);
    pDone->addWidget(m_pCardDoneText);
    m_pCardDone->setVisible(false);

    pCardLayout->addWidget(m_pCardForm);
    pCardLayout->addWidget(m_pCardDone);
    pCardOuter->addStretch(1);
    pCardOuter->addWidget(pCard, 0, Qt::AlignTop);
    pCardOuter->addStretch(1);
    m_pProfileCard->setVisible(false);
    pMain->addWidget(m_pProfileCard);

    m_pTable = new SoundCloudTable(0, 4, pPanel);
    m_pTable->setObjectName(QStringLiteral("scTable"));
    m_pTable->setFrameShape(QFrame::NoFrame);
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->setDragEnabled(true);
    m_pTable->setDragDropMode(QAbstractItemView::DragOnly);
    m_pTable->setMouseTracking(true);
    m_pTable->setShowGrid(false);
    m_pTable->setWordWrap(false);
    m_pTable->setFocusPolicy(Qt::StrongFocus);
    m_pTable->verticalHeader()->setVisible(false);
    m_pTable->verticalHeader()->setDefaultSectionSize(56);
    m_pTable->horizontalHeader()->setVisible(false);
    m_pTable->horizontalHeader()->setSectionResizeMode(kColLoad, QHeaderView::Fixed);
    m_pTable->horizontalHeader()->setSectionResizeMode(kColTrack, QHeaderView::Stretch);
    m_pTable->horizontalHeader()->setSectionResizeMode(kColDuration, QHeaderView::Fixed);
    m_pTable->horizontalHeader()->setSectionResizeMode(kColAction, QHeaderView::Fixed);
    m_pTable->setColumnWidth(kColLoad, 72);
    m_pTable->setColumnWidth(kColDuration, 70);
    m_pTable->setColumnWidth(kColAction, 150);
    // Pantallas tactiles: desplazamiento con el dedo (el mouse sigue arrastrando a los decks)
    QScroller::grabGesture(m_pTable->viewport(), QScroller::TouchGesture);
    m_pTable->setItemDelegateForColumn(kColLoad, new LoadDelegate(m_pTable));
    m_pTable->viewport()->setAttribute(Qt::WA_Hover, true);
    m_pTable->setItemDelegateForColumn(kColTrack, new TrackDelegate(m_pTable));
    m_pTable->setItemDelegateForColumn(kColAction, new ActionDelegate(m_pTable));
    pMain->addWidget(m_pTable, 1);

    // Vista grilla: misma informacion, otra forma de verla
    m_pGrid = new SoundCloudGrid(pPanel);
    m_pGrid->setObjectName(QStringLiteral("scGrid"));
    m_pGrid->setModel(m_pTable->model());
    m_pGrid->setModelColumn(kColTrack);
    m_pGrid->setSelectionModel(m_pTable->selectionModel());
    m_pGrid->setViewMode(QListView::IconMode);
    m_pGrid->setResizeMode(QListView::Adjust);
    m_pGrid->setMovement(QListView::Static);
    m_pGrid->setUniformItemSizes(true);
    m_pGrid->setSpacing(6);
    m_pGrid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pGrid->setDragEnabled(true);
    m_pGrid->setDragDropMode(QAbstractItemView::DragOnly);
    m_pGrid->setMouseTracking(true);
    m_pGrid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_pGrid->setFrameShape(QFrame::NoFrame);
    m_pGrid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pGrid->setItemDelegate(new GridDelegate(m_pGrid));
    QScroller::grabGesture(m_pGrid->viewport(), QScroller::TouchGesture);
    pMain->addWidget(m_pGrid, 1);
    connect(m_pGrid, &QListView::clicked, this, &DlgSoundCloud::slotGridClicked);
    connect(m_pGrid, &QListView::doubleClicked, this, &DlgSoundCloud::slotGridDoubleClicked);

    auto* pFooter = new QHBoxLayout();
    m_pStatusLabel = new QLabel(pPanel);
    m_pStatusLabel->setObjectName(QStringLiteral("scStatus"));
    m_pStatusLabel->setWordWrap(true);
    m_pProgress = new QProgressBar(pPanel);
    m_pProgress->setObjectName(QStringLiteral("scProgress"));
    m_pProgress->setRange(0, 100);
    m_pProgress->setTextVisible(false);
    m_pProgress->setFixedWidth(160);
    m_pProgress->setVisible(false);
    pFooter->addWidget(m_pStatusLabel, 1);
    pFooter->addWidget(m_pProgress, 0, Qt::AlignVCenter);
    pMain->addLayout(pFooter);

    pRoot->addWidget(pPanel, 1);

    connect(m_pNav, &QListWidget::currentRowChanged, this, &DlgSoundCloud::slotNavChanged);
    connect(m_pSearchEdit, &QLineEdit::returnPressed, this, &DlgSoundCloud::slotSearch);
    connect(m_pPlaylistCombo,
            QOverload<int>::of(&QComboBox::activated),
            this,
            &DlgSoundCloud::slotPlaylistChosen);
    connect(m_pProfileButton, &QPushButton::clicked, this, &DlgSoundCloud::slotChangeProfile);
    connect(m_pConnectButton, &QPushButton::clicked, this, &DlgSoundCloud::slotConnectProfile);
    connect(m_pCancelButton, &QPushButton::clicked, this, &DlgSoundCloud::slotCancelProfile);
    connect(m_pProfileEdit, &QLineEdit::returnPressed, this, &DlgSoundCloud::slotConnectProfile);
    connect(m_pProfileEdit, &QLineEdit::textEdited, this, [this]() { setProfileError(QString()); });
    updateProfileButton();
    connect(m_pGridButton, &QToolButton::clicked, this, [this]() { setGridMode(true); });
    connect(m_pListButton, &QToolButton::clicked, this, [this]() { setGridMode(false); });
    setGridMode(m_pConfig->getValueString(kViewModeKey) != QStringLiteral("list"));

    // Soltar sobre un deck un tema que todavia no esta descargado
    QPointer<DlgSoundCloud> self(this);
    DragAndDropHelper::setRemoteDropHandler([self](const QMimeData& mimeData, const QString& group) {
        if (!self) {
            return false;
        }
        const QString url = QString::fromUtf8(mimeData.data(DragAndDropHelper::kRemoteTrackMimeType));
        return self->loadUrlToGroup(url, group);
    });
    // Indicadores A/B: que deck tiene cargado cada tema
    if (m_pLibrary) {
        connect(&PlayerInfo::instance(),
                &PlayerInfo::trackChanged,
                this,
                [this]() { refreshDeckIndicators(); });
    }
    connect(m_pTable, &QTableWidget::cellClicked, this, &DlgSoundCloud::slotCellClicked);
    connect(m_pTable,
            &QTableWidget::cellDoubleClicked,
            this,
            &DlgSoundCloud::slotCellDoubleClicked);
    connect(m_pDownloadButton,
            &QPushButton::clicked,
            this,
            &DlgSoundCloud::slotDownloadSelected);
    connect(m_pDownloadAllButton, &QPushButton::clicked, this, &DlgSoundCloud::slotDownloadAll);
    connect(m_pDeck1Button, &QPushButton::clicked, this, &DlgSoundCloud::slotLoadDeck1);
    connect(m_pDeck2Button, &QPushButton::clicked, this, &DlgSoundCloud::slotLoadDeck2);
    connect(m_pFolderButton, &QPushButton::clicked, this, &DlgSoundCloud::slotChooseFolder);
    connect(m_pOpenFolderButton, &QPushButton::clicked, this, &DlgSoundCloud::slotOpenFolder);

    setHeader(tr("Buscar"), tr("Encontrá cualquier tema de SoundCloud y llevalo al deck"));
    setStatus(tr("Doble click en un tema: se descarga y se carga en el primer deck libre. "
                 "Los descargados se pueden arrastrar a cualquier deck."));
}

DlgSoundCloud::~DlgSoundCloud() {
    DragAndDropHelper::setRemoteDropHandler(nullptr);
    for (QProcess* pProcess : {m_pListProcess, m_pDownloadProcess}) {
        if (pProcess && pProcess->state() != QProcess::NotRunning) {
            pProcess->disconnect(this);
            pProcess->kill();
            pProcess->waitForFinished(1000);
        }
    }
}

void DlgSoundCloud::onShow() {
    if (m_page == Page::Search) {
        m_pSearchEdit->setFocus();
    }
}

bool DlgSoundCloud::hasFocus() const {
    return m_pSearchEdit->hasFocus() || m_pTable->hasFocus() || m_pGrid->hasFocus() || m_pNav->hasFocus();
}

void DlgSoundCloud::setFocus() {
    // Con resultados en pantalla el foco va a la lista, asi un controlador
    // puede navegarla ([Library],MoveVertical) y cargar al deck.
    if (m_pTable->rowCount() > 0) {
        if (m_gridMode) {
            m_pGrid->setFocus();
        } else {
            m_pTable->setFocus();
        }
        if (m_pTable->currentRow() < 0) {
            m_pTable->selectRow(0);
        }
    } else {
        m_pSearchEdit->setFocus();
    }
}

// ----------------------------------------------------------- controladores

bool DlgSoundCloud::loadSelectedToGroup(const QString& group, bool play) {
    const QList<int> rows = selectedRows();
    int row = rows.isEmpty() ? m_pTable->currentRow() : rows.first();
    if (row < 0) {
        return false;
    }
    const QTableWidgetItem* pItem = m_pTable->item(row, kColTrack);
    if (!pItem) {
        return false;
    }
    const bool ok = loadUrlToGroup(pItem->data(kUrlRole).toString(), group);
    if (ok) {
        m_groupTargets[pItem->data(kUrlRole).toString()].second = play;
    }
    return ok;
}

bool DlgSoundCloud::activateSelected() {
    const int row = m_pTable->currentRow();
    if (row < 0) {
        return false;
    }
    enqueue(row, After::AutoDeck);
    return true;
}

// ------------------------------------------------------------------ rutas

QString DlgSoundCloud::bridgeDir() const {
    const QString configured = m_pConfig->getValueString(kBridgeDirKey);
    if (!configured.isEmpty()) {
        return configured;
    }
    return QDir::homePath() + QStringLiteral("/Downloads/SCDL");
}

QString DlgSoundCloud::bridgeScript() const {
    return QDir(bridgeDir()).filePath(QStringLiteral("scdl_bridge.py"));
}

QString DlgSoundCloud::pythonPath() const {
    const QDir dir(bridgeDir());
#ifdef Q_OS_WIN
    const QString venvPython = dir.filePath(QStringLiteral(".venv/Scripts/python.exe"));
#else
    const QString venvPython = dir.filePath(QStringLiteral(".venv/bin/python"));
#endif
    if (QFileInfo::exists(venvPython)) {
        return venvPython;
    }
#ifdef Q_OS_WIN
    return QStringLiteral("python");
#else
    return QStringLiteral("python3");
#endif
}

QString DlgSoundCloud::downloadDir() const {
    const QString configured = m_pConfig->getValueString(kDownloadDirKey);
    if (!configured.isEmpty()) {
        return configured;
    }
    return QStandardPaths::writableLocation(QStandardPaths::MusicLocation) +
            QStringLiteral("/scdl");
}

QString DlgSoundCloud::profile(bool askIfMissing) {
    Q_UNUSED(askIfMissing);
    return m_pConfig->getValueString(kProfileKey);
}

QString DlgSoundCloud::normalizeProfile(const QString& input) {
    QString value = input.trimmed();
    if (value.startsWith(QLatin1Char('@'))) {
        value.remove(0, 1);
    }
    if (value.isEmpty()) {
        return QString();
    }
    if (!value.contains(QStringLiteral("soundcloud.com"))) {
        return QStringLiteral("https://soundcloud.com/") + value;
    }
    if (!value.startsWith(QStringLiteral("http"))) {
        value = QStringLiteral("https://") + value;
    }
    // Quedarse solo con el usuario: https://soundcloud.com/usuario
    const QUrl url(value);
    const QString user = url.path().section(QLatin1Char('/'), 1, 1);
    return user.isEmpty() ? QString() : QStringLiteral("https://soundcloud.com/") + user;
}

QProcessEnvironment DlgSoundCloud::processEnvironment() const {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
#ifdef Q_OS_MACOS
    // Las apps de macOS arrancan con un PATH minimo: sumar Homebrew (ffmpeg)
    env.insert(QStringLiteral("PATH"),
            QStringLiteral("/opt/homebrew/bin:/usr/local/bin:") +
                    env.value(QStringLiteral("PATH")));
#endif
    return env;
}

bool DlgSoundCloud::ensureBridge() {
    if (QFileInfo::exists(bridgeScript())) {
        return true;
    }
    setStatus(tr("No encuentro scdl_bridge.py en %1. Elegí la carpeta de SCDL.")
                      .arg(QDir::toNativeSeparators(bridgeDir())),
            true);
    const QString dir = QFileDialog::getExistingDirectory(this,
            tr("Carpeta de SCDL (la que tiene scdl_bridge.py)"),
            QDir::homePath());
    if (dir.isEmpty()) {
        return false;
    }
    m_pConfig->setValue(kBridgeDirKey, dir);
    if (!QFileInfo::exists(bridgeScript())) {
        setStatus(tr("Esa carpeta no tiene scdl_bridge.py."), true);
        return false;
    }
    return true;
}

// ------------------------------------------------------------- navegacion

void DlgSoundCloud::setHeader(const QString& title, const QString& subtitle) {
    m_pTitle->setText(title);
    m_pSubtitle->setText(subtitle);
}

void DlgSoundCloud::slotNavChanged(int index) {
    openPage(static_cast<Page>(index));
}

void DlgSoundCloud::openPage(Page page) {
    m_page = page;
    hideProfileCard();
    m_pSearchEdit->setVisible(m_page == Page::Search);
    m_pPlaylistCombo->setVisible(m_page == Page::Playlists && m_pPlaylistCombo->count() > 0);
    const bool needsProfile = m_page == Page::Likes || m_page == Page::Playlists;
    if (needsProfile && profile(false).isEmpty()) {
        m_pendingPage = m_page;
        setHeader(m_page == Page::Likes ? tr("Mis likes") : tr("Mis playlists"),
                tr("Conectá tu perfil para verlos acá"));
        showProfileCard(false);
        return;
    }
    const QString user = m_pConfig->getValueString(kProfileNameKey);
    switch (m_page) {
    case Page::Search:
        setHeader(tr("Buscar"), tr("Encontrá cualquier tema de SoundCloud y llevalo al deck"));
        m_pTable->setRowCount(0);
        m_pSearchEdit->setFocus();
        if (!m_pSearchEdit->text().trimmed().isEmpty()) {
            slotSearch();
        }
        break;
    case Page::Likes:
        setHeader(tr("Mis likes"), user.isEmpty() ? QString() : QStringLiteral("@") + user);
        m_pTable->setRowCount(0);
        runList({QStringLiteral("likes"), profile(false), QStringLiteral("300")},
                ListKind::Tracks,
                tr("Cargando tus likes…"));
        break;
    case Page::Playlists:
        setHeader(tr("Mis playlists"), tr("Elegí una playlist para ver sus temas"));
        m_pTable->setRowCount(0);
        runList({QStringLiteral("playlists"), profile(false)},
                ListKind::Playlists,
                tr("Cargando tus playlists…"));
        break;
    case Page::Downloads:
        showDownloadsPage();
        break;
    }
}

// ------------------------------------------------------------- perfil

void DlgSoundCloud::showProfileCard(bool changing) {
    m_pCardTitle->setText(changing ? tr("Cambiar perfil de SoundCloud") : tr("Conectá tu SoundCloud"));
    m_pProfileEdit->setText(changing ? m_pConfig->getValueString(kProfileKey) : QString());
    m_pCancelButton->setVisible(true);
    m_pConnectButton->setEnabled(true);
    m_pConnectButton->setText(changing ? tr("Guardar") : tr("Conectar"));
    m_pCardForm->setVisible(true);
    m_pCardDone->setVisible(false);
    setProfileError(QString());
    setResultsVisible(false);
    m_pSearchEdit->setVisible(false);
    m_pPlaylistCombo->setVisible(false);
    for (QPushButton* pButton : {m_pDeck1Button, m_pDeck2Button, m_pDownloadButton, m_pDownloadAllButton}) {
        pButton->setVisible(false);
    }
    m_pGridButton->setVisible(false);
    m_pListButton->setVisible(false);
    m_pProfileCard->setVisible(true);
    m_pProfileEdit->setFocus();
    m_pProfileEdit->selectAll();
    setStatus(QString());
}

void DlgSoundCloud::hideProfileCard() {
    m_pProfileCard->setVisible(false);
    setResultsVisible(true);
    for (QPushButton* pButton : {m_pDeck1Button, m_pDeck2Button, m_pDownloadButton, m_pDownloadAllButton}) {
        pButton->setVisible(true);
    }
    m_pGridButton->setVisible(true);
    m_pListButton->setVisible(true);
}

void DlgSoundCloud::setProfileError(const QString& text) {
    m_pProfileError->setText(text);
    m_pProfileError->setVisible(!text.isEmpty());
    m_pProfileEdit->setProperty("invalid", !text.isEmpty());
    m_pProfileEdit->style()->unpolish(m_pProfileEdit);
    m_pProfileEdit->style()->polish(m_pProfileEdit);
}

void DlgSoundCloud::updateProfileButton() {
    const QString user = m_pConfig->getValueString(kProfileNameKey);
    if (m_pConfig->getValueString(kProfileKey).isEmpty()) {
        m_pProfileButton->setText(QStringLiteral("＋  ") + tr("Conectar SoundCloud"));
        m_pProfileButton->setToolTip(tr("Conectá tu perfil para ver tus likes y playlists"));
    } else {
        m_pProfileButton->setText(QStringLiteral("@%1  ·  %2")
                                          .arg(user.isEmpty() ? tr("perfil") : user, tr("Cambiar")));
        m_pProfileButton->setToolTip(m_pConfig->getValueString(kProfileKey));
    }
}

void DlgSoundCloud::slotConnectProfile() {
    const QString url = normalizeProfile(m_pProfileEdit->text());
    if (url.isEmpty()) {
        setProfileError(tr("Escribí tu usuario o pegá el link de tu perfil."));
        return;
    }
    setProfileError(QString());
    m_pConnectButton->setEnabled(false);
    m_pConnectButton->setText(tr("Verificando…"));
    m_pCancelButton->setVisible(false);
    runList({QStringLiteral("profile"), url}, ListKind::Profile, tr("Verificando tu perfil…"));
}

void DlgSoundCloud::slotCancelProfile() {
    hideProfileCard();
    if (profile(false).isEmpty() && (m_page == Page::Likes || m_page == Page::Playlists)) {
        m_pNav->setCurrentRow(static_cast<int>(Page::Search));
    } else {
        openPage(m_page);
    }
}

void DlgSoundCloud::slotSearch() {
    const QString query = m_pSearchEdit->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    setHeader(tr("Resultados"), QStringLiteral("“%1”").arg(query));
    runList({QStringLiteral("search"), query, QStringLiteral("40")},
            ListKind::Tracks,
            tr("Buscando “%1”…").arg(query));
}

void DlgSoundCloud::slotPlaylistChosen(int index) {
    const QString url = m_pPlaylistCombo->itemData(index).toString();
    if (url.isEmpty()) {
        return;
    }
    setHeader(m_pPlaylistCombo->itemText(index).section(QStringLiteral("  ("), 0, 0),
            tr("Playlist"));
    runList({QStringLiteral("playlist"), url}, ListKind::Tracks, tr("Cargando playlist…"));
}

void DlgSoundCloud::slotChangeProfile() {
    m_pendingPage = (m_page == Page::Likes || m_page == Page::Playlists) ? m_page : Page::Likes;
    setHeader(tr("Tu perfil"), tr("Conectá o cambiá tu cuenta de SoundCloud"));
    showProfileCard(!profile(false).isEmpty());
}

void DlgSoundCloud::setBusy(bool busy) {
    m_pNav->setEnabled(!busy);
    m_pProfileButton->setEnabled(!busy);
    m_pSearchEdit->setEnabled(!busy);
}

void DlgSoundCloud::showDownloadsPage() {
    setHeader(tr("Descargas"),
            m_sessionDownloads.isEmpty()
                    ? tr("Todavía no descargaste nada en esta sesión")
                    : tr("%1 temas en esta sesión").arg(m_sessionDownloads.size()));
    QJsonArray tracks;
    for (const QString& url : std::as_const(m_sessionDownloads)) {
        tracks.append(m_trackInfo.value(url));
    }
    fillTracks(tracks);
    setStatus(tr("Carpeta: %1").arg(QDir::toNativeSeparators(downloadDir())));
}

// ------------------------------------------------------------- listados

void DlgSoundCloud::runList(const QStringList& args, ListKind kind, const QString& busyText) {
    if (!ensureBridge()) {
        return;
    }
    if (m_pListProcess) {
        m_pListProcess->disconnect(this);
        m_pListProcess->kill();
        m_pListProcess->deleteLater();
        m_pListProcess = nullptr;
    }
    m_listKind = kind;
    m_pListProcess = new QProcess(this);
    m_pListProcess->setProcessEnvironment(processEnvironment());
    m_pListProcess->setWorkingDirectory(bridgeDir());
    connect(m_pListProcess, &QProcess::finished, this, &DlgSoundCloud::slotListFinished);
    connect(m_pListProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setBusy(false);
            setStatus(tr("No pude iniciar Python (%1). Abrí SCDL una vez para que "
                         "se instale.")
                              .arg(QDir::toNativeSeparators(pythonPath())),
                    true);
        }
    });
    setBusy(true);
    setStatus(busyText);
    QStringList fullArgs{bridgeScript()};
    fullArgs.append(args);
    m_pListProcess->start(pythonPath(), fullArgs);
}

void DlgSoundCloud::slotListFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);
    setBusy(false);
    if (!m_pListProcess) {
        return;
    }
    const QByteArray output = m_pListProcess->readAllStandardOutput();
    const QByteArray errors = m_pListProcess->readAllStandardError();
    m_pListProcess->deleteLater();
    m_pListProcess = nullptr;

    QJsonObject result;
    for (const QByteArray& line : output.split('\n')) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
        if (doc.isObject()) {
            result = doc.object();
        }
    }
    if (m_listKind == ListKind::Profile) {
        m_pConnectButton->setEnabled(true);
        m_pConnectButton->setText(tr("Conectar"));
        m_pCancelButton->setVisible(true);
        if (!result.contains(QStringLiteral("user"))) {
            const QString err = result.value(QStringLiteral("error")).toString();
            setProfileError(err.contains(QStringLiteral("perfil"), Qt::CaseInsensitive) || err.isEmpty()
                            ? tr("No encontramos ese perfil. Revisá el usuario o el link.")
                            : tr("No pudimos verificarlo: %1").arg(err));
            setStatus(QString());
            return;
        }
        const QJsonObject user = result.value(QStringLiteral("user")).toObject();
        m_pConfig->setValue(kProfileKey, user.value(QStringLiteral("url")).toString());
        m_pConfig->setValue(kProfileNameKey, user.value(QStringLiteral("username")).toString());
        updateProfileButton();
        m_pCardForm->setVisible(false);
        m_pCardDoneText->setText(tr("Conectado como @%1").arg(user.value(QStringLiteral("username")).toString()));
        m_pCardDone->setVisible(true);
        setStatus(QString());
        const Page next = m_pendingPage;
        QTimer::singleShot(1100, this, [this, next]() {
            if (m_pNav->currentRow() != static_cast<int>(next)) {
                m_pNav->setCurrentRow(static_cast<int>(next)); // dispara openPage
            } else {
                openPage(next);
            }
        });
        return;
    }
    if (result.contains(QStringLiteral("error"))) {
        setStatus(tr("Error: %1").arg(result.value(QStringLiteral("error")).toString()), true);
        return;
    }
    if (m_listKind == ListKind::Playlists && result.contains(QStringLiteral("playlists"))) {
        fillPlaylists(result.value(QStringLiteral("playlists")).toArray());
        return;
    }
    if (!result.contains(QStringLiteral("results"))) {
        const QString detail = QString::fromUtf8(errors).trimmed().section('\n', -1);
        setStatus(tr("No se pudo cargar. %1").arg(detail), true);
        return;
    }
    const QJsonArray results = result.value(QStringLiteral("results")).toArray();
    fillTracks(results);
    if (m_page == Page::Likes) {
        setHeader(tr("Mis likes"), tr("%1 temas").arg(results.size()));
    } else if (m_page == Page::Playlists) {
        m_pSubtitle->setText(tr("Playlist  ·  %1 temas").arg(results.size()));
    } else if (m_page == Page::Search) {
        m_pSubtitle->setText(QStringLiteral("“%1”  ·  %2 temas")
                                     .arg(m_pSearchEdit->text().trimmed())
                                     .arg(results.size()));
    }
    setStatus(results.isEmpty()
                    ? tr("Sin resultados.")
                    : tr("Doble click: descargar y cargar en el primer deck libre  ·  "
                         "click en ↓: solo descargar  ·  arrastrá los listos a un deck"));
}

void DlgSoundCloud::fillPlaylists(const QJsonArray& playlists) {
    m_pPlaylistCombo->clear();
    for (const QJsonValue& value : playlists) {
        const QJsonObject pl = value.toObject();
        m_pPlaylistCombo->addItem(QStringLiteral("%1  (%2)")
                                          .arg(pl.value(QStringLiteral("title")).toString())
                                          .arg(pl.value(QStringLiteral("count")).toInt()),
                pl.value(QStringLiteral("url")).toString());
    }
    m_pPlaylistCombo->setCurrentIndex(-1);
    m_pPlaylistCombo->setVisible(true);
    if (playlists.isEmpty()) {
        setStatus(tr("No encontré playlists públicas en tu perfil."));
    } else {
        setStatus(tr("%1 playlists. Elegí una en la lista.").arg(playlists.size()));
        m_pPlaylistCombo->showPopup();
    }
}

void DlgSoundCloud::fillTracks(const QJsonArray& results) {
    m_pTable->setRowCount(0);
    m_pTable->setRowCount(static_cast<int>(results.size()));
    int row = 0;
    for (const QJsonValue& value : results) {
        const QJsonObject track = value.toObject();
        const QString url = track.value(QStringLiteral("url")).toString();
        m_trackInfo.insert(url, track);

        auto* pTrack = new QTableWidgetItem(track.value(QStringLiteral("title")).toString());
        pTrack->setData(kUrlRole, url);
        pTrack->setData(kArtistRole, track.value(QStringLiteral("artist")).toString());
        pTrack->setData(kPreviewRole, track.value(QStringLiteral("preview")).toBool());
        pTrack->setToolTip(url);
        auto* pDuration = new QTableWidgetItem(formatDuration(static_cast<qint64>(
                track.value(QStringLiteral("duration_ms")).toDouble())));
        pDuration->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pDuration->setForeground(kMuted);
        auto* pAction = new QTableWidgetItem();
        pAction->setToolTip(tr("Descargar"));
        auto* pLoad = new QTableWidgetItem();
        pLoad->setToolTip(tr("Cargar en Deck A o B"));
        m_pTable->setItem(row, kColLoad, pLoad);
        pTrack->setData(kTargetRole, m_targetMask.value(url, 0));
        m_pTable->setItem(row, kColTrack, pTrack);
        m_pTable->setItem(row, kColDuration, pDuration);
        m_pTable->setItem(row, kColAction, pAction);

        const QString path = pathForUrl(url);
        if (!path.isEmpty()) {
            pTrack->setData(kPathRole, path);
            m_state.insert(url, kStateDone);
        }
        pTrack->setData(kStateRole, m_state.value(url, kStateNone));
        pTrack->setData(kProgressRole, m_progress.value(url, 0));
        if (m_artwork.contains(url)) {
            pTrack->setData(Qt::DecorationRole, m_artwork.value(url));
            pTrack->setData(kBigCoverRole, m_artworkBig.value(url));
        } else {
            loadArtwork(url, track.value(QStringLiteral("artwork")).toString());
        }
        ++row;
    }
    refreshDeckIndicators();
}

void DlgSoundCloud::loadArtwork(const QString& trackUrl, const QString& artUrl) {
    if (artUrl.isEmpty()) {
        return;
    }
    // Pedir una portada un poco mas grande que la de 100 px por defecto
    QString url = artUrl;
    url.replace(QStringLiteral("-large."), QStringLiteral("-t300x300."));
    QNetworkReply* pReply = m_pNetwork->get(QNetworkRequest(QUrl(url)));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, trackUrl]() {
        pReply->deleteLater();
        if (pReply->error() != QNetworkReply::NoError) {
            return;
        }
        QPixmap pixmap;
        if (!pixmap.loadFromData(pReply->readAll())) {
            return;
        }
        const QPixmap rounded = roundedPixmap(pixmap, 38, 6);
        const QPixmap big = pixmap.scaled(kGridCover * 2, kGridCover * 2,
                Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        m_artwork.insert(trackUrl, rounded);
        m_artworkBig.insert(trackUrl, big);
        const int row = rowForUrl(trackUrl);
        if (row >= 0) {
            m_pTable->item(row, kColTrack)->setData(Qt::DecorationRole, rounded);
            m_pTable->item(row, kColTrack)->setData(kBigCoverRole, big);
        }
    });
}

// ------------------------------------------------------------- descargas

int DlgSoundCloud::rowForUrl(const QString& url) const {
    for (int row = 0; row < m_pTable->rowCount(); ++row) {
        const QTableWidgetItem* pItem = m_pTable->item(row, kColTrack);
        if (pItem && pItem->data(kUrlRole).toString() == url) {
            return row;
        }
    }
    return -1;
}

QString DlgSoundCloud::pathForUrl(const QString& url) const {
    const QString path = m_pConfig->getValueString(downloadedKey(url));
    return (!path.isEmpty() && QFileInfo::exists(path)) ? path : QString();
}

QList<int> DlgSoundCloud::selectedRows() const {
    QList<int> rows;
    // la grilla selecciona una sola columna: tomar las filas de cualquier indice
    const QModelIndexList selected = m_pTable->selectionModel()->selectedIndexes();
    for (const QModelIndex& index : selected) {
        if (!rows.contains(index.row())) {
            rows.append(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

void DlgSoundCloud::slotCellClicked(int row, int column) {
    if (column == kColLoad) {
        const QRect cell = m_pTable->visualRect(m_pTable->model()->index(row, kColLoad));
        const int deck = deckChipAt(cell, m_pTable->viewport()->mapFromGlobal(QCursor::pos()));
        const QTableWidgetItem* pTrack = m_pTable->item(row, kColTrack);
        if (deck >= 0 && pTrack) {
            loadUrlToGroup(pTrack->data(kUrlRole).toString(), kDeckGroups[deck]);
        }
        return;
    }
    if (column != kColAction) {
        return;
    }
    const QTableWidgetItem* pItem = m_pTable->item(row, kColTrack);
    if (!pItem) {
        return;
    }
    const int state = pItem->data(kStateRole).toInt();
    if (state == kStateNone || state == kStateError) {
        enqueue(row, After::Nothing);
    }
}

void DlgSoundCloud::slotCellDoubleClicked(int row, int column) {
    if (column == kColAction || column == kColLoad) {
        return;
    }
    enqueue(row, After::AutoDeck);
}

void DlgSoundCloud::slotDownloadSelected() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        setStatus(tr("Seleccioná uno o más temas de la lista."));
        return;
    }
    for (int row : rows) {
        enqueue(row, After::Nothing);
    }
}

void DlgSoundCloud::slotDownloadAll() {
    for (int row = 0; row < m_pTable->rowCount(); ++row) {
        enqueue(row, After::Nothing);
    }
}

void DlgSoundCloud::slotLoadDeck1() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        setStatus(tr("Elegí un tema de la lista."));
        return;
    }
    const QTableWidgetItem* pTrack = m_pTable->item(rows.first(), kColTrack);
    if (pTrack) {
        loadUrlToGroup(pTrack->data(kUrlRole).toString(), QStringLiteral("[Channel1]"));
    }
}

void DlgSoundCloud::slotLoadDeck2() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        setStatus(tr("Elegí un tema de la lista."));
        return;
    }
    const QTableWidgetItem* pTrack = m_pTable->item(rows.first(), kColTrack);
    if (pTrack) {
        loadUrlToGroup(pTrack->data(kUrlRole).toString(), QStringLiteral("[Channel2]"));
    }
}

void DlgSoundCloud::enqueue(int row, After after) {
    const QTableWidgetItem* pItem = m_pTable->item(row, kColTrack);
    if (!pItem) {
        return;
    }
    enqueueUrl(pItem->data(kUrlRole).toString(), after);
}

bool DlgSoundCloud::loadUrlToGroup(const QString& url, const QString& group) {
    if (url.isEmpty() || group.isEmpty()) {
        return false;
    }
    m_groupTargets.insert(url, qMakePair(group, false));
    const int bit = deckBit(group);
    if (pathForUrl(url).isEmpty()) {
        setTargetDeck(url, m_targetMask.value(url, 0) | bit);
        const QString title = m_trackInfo.value(url).value(QStringLiteral("title")).toString();
        const QString deckName = bit == 1 ? QStringLiteral("A") : bit == 2 ? QStringLiteral("B") : group;
        setStatus(tr("Descargando “%1” para el Deck %2…").arg(title, deckName));
    }
    enqueueUrl(url, After::Group);
    return true;
}

void DlgSoundCloud::enqueueUrl(const QString& url, After after) {
    if (url.isEmpty()) {
        return;
    }
    if (!pathForUrl(url).isEmpty()) {
        handleAfter(url, after); // ya descargado
        return;
    }
    if (url == m_currentUrl) {
        if (after != After::Nothing) {
            m_currentAfter = after;
        }
        return;
    }
    for (auto& entry : m_queue) {
        if (entry.first == url) {
            if (after != After::Nothing) {
                entry.second = after;
            }
            return;
        }
    }
    // Los pedidos para un deck van primero en la cola
    if (after != After::Nothing) {
        m_queue.prepend(qMakePair(url, after));
    } else {
        m_queue.append(qMakePair(url, after));
    }
    if (!m_sessionDownloads.contains(url)) {
        m_sessionDownloads.append(url);
    }
    setUrlState(url, kStateQueued);
    if (!m_pDownloadProcess) {
        startNextDownload();
    }
}

void DlgSoundCloud::setTargetDeck(const QString& url, int mask) {
    if (mask) {
        m_targetMask.insert(url, mask);
    } else {
        m_targetMask.remove(url);
    }
    const int row = rowForUrl(url);
    if (row >= 0) {
        m_pTable->item(row, kColTrack)->setData(kTargetRole, mask);
        m_pTable->viewport()->update(m_pTable->visualRect(m_pTable->model()->index(row, kColLoad)));
    }
}

void DlgSoundCloud::refreshDeckIndicators() {
    if (!m_pLibrary) {
        return;
    }
    QString loaded[2];
    for (int i = 0; i < 2; ++i) {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(kDeckGroups[i]);
        loaded[i] = pTrack ? pTrack->getLocation() : QString();
    }
    for (int row = 0; row < m_pTable->rowCount(); ++row) {
        QTableWidgetItem* pTrack = m_pTable->item(row, kColTrack);
        if (!pTrack) {
            continue;
        }
        const QString path = pTrack->data(kPathRole).toString();
        int mask = 0;
        for (int i = 0; i < 2; ++i) {
            if (samePath(path, loaded[i])) {
                mask |= 1 << i;
            }
        }
        pTrack->setData(kDecksRole, mask);
    }
    m_pTable->viewport()->update();
}

void DlgSoundCloud::startNextDownload() {
    if (m_queue.isEmpty()) {
        m_pProgress->setVisible(false);
        return;
    }
    if (!ensureBridge()) {
        m_queue.clear();
        return;
    }
    const auto next = m_queue.takeFirst();
    m_currentUrl = next.first;
    m_currentAfter = next.second;
    const QString folder = downloadDir();
    QDir().mkpath(folder);

    m_downloadBuffer.clear();
    m_lastError.clear();
    m_pDownloadProcess = new QProcess(this);
    m_pDownloadProcess->setProcessEnvironment(processEnvironment());
    m_pDownloadProcess->setWorkingDirectory(bridgeDir());
    connect(m_pDownloadProcess,
            &QProcess::readyReadStandardOutput,
            this,
            &DlgSoundCloud::slotDownloadOutput);
    connect(m_pDownloadProcess,
            &QProcess::finished,
            this,
            &DlgSoundCloud::slotDownloadFinished);
    connect(m_pDownloadProcess,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    m_lastError = tr("No pude iniciar Python");
                    slotDownloadFinished(-1, QProcess::CrashExit);
                }
            });

    setUrlState(m_currentUrl, kStateDownloading, 0);
    m_pProgress->setValue(0);
    m_pProgress->setVisible(true);
    const QString title = m_trackInfo.value(m_currentUrl).value(QStringLiteral("title")).toString();
    setStatus(m_queue.isEmpty()
                    ? tr("Descargando “%1”").arg(title)
                    : tr("Descargando “%1”  ·  %2 en cola").arg(title).arg(m_queue.size()));
    m_pDownloadProcess->start(pythonPath(),
            {bridgeScript(), QStringLiteral("download"), m_currentUrl, folder});
}

void DlgSoundCloud::slotDownloadOutput() {
    if (!m_pDownloadProcess) {
        return;
    }
    m_downloadBuffer += m_pDownloadProcess->readAllStandardOutput();
    int newline;
    while ((newline = m_downloadBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_downloadBuffer.left(newline).trimmed();
        m_downloadBuffer.remove(0, newline + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject event = doc.object();
        if (event.contains(QStringLiteral("progress"))) {
            const int pct = static_cast<int>(event.value(QStringLiteral("progress")).toDouble());
            m_pProgress->setValue(pct);
            setUrlState(m_currentUrl, kStateDownloading, pct);
            const QString title =
                    m_trackInfo.value(m_currentUrl).value(QStringLiteral("title")).toString();
            QString status = tr("Descargando “%1”  ·  %2%").arg(title).arg(pct);
            const QString speed = event.value(QStringLiteral("speed")).toString();
            const QString eta = event.value(QStringLiteral("eta")).toString();
            if (!speed.isEmpty()) {
                status += QStringLiteral("  ·  ") + speed;
            }
            if (!eta.isEmpty()) {
                status += tr("  ·  quedan %1").arg(eta);
            }
            if (!m_queue.isEmpty()) {
                status += tr("  ·  %1 en cola").arg(m_queue.size());
            }
            setStatus(status);
        } else if (event.contains(QStringLiteral("status"))) {
            setUrlState(m_currentUrl, kStateProcessing, 100);
            m_pProgress->setValue(100);
        } else if (event.contains(QStringLiteral("done"))) {
            finishDownload(m_currentUrl, event.value(QStringLiteral("done")).toString());
        } else if (event.contains(QStringLiteral("error"))) {
            m_lastError = event.value(QStringLiteral("error")).toString();
        }
    }
}

void DlgSoundCloud::slotDownloadFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (!m_pDownloadProcess) {
        return;
    }
    slotDownloadOutput();
    QProcess* pProcess = m_pDownloadProcess;
    m_pDownloadProcess = nullptr;
    pProcess->disconnect(this);
    const QString stderrText = QString::fromUtf8(pProcess->readAllStandardError()).trimmed();
    pProcess->deleteLater();

    if (pathForUrl(m_currentUrl).isEmpty()) {
        QString reason = m_lastError;
        if (reason.isEmpty()) {
            reason = stderrText.section('\n', -1);
        }
        if (reason.isEmpty() && (exitCode != 0 || exitStatus != QProcess::NormalExit)) {
            reason = tr("el proceso terminó con código %1").arg(exitCode);
        }
        setUrlState(m_currentUrl, kStateError);
        setTargetDeck(m_currentUrl, 0);
        const int row = rowForUrl(m_currentUrl);
        if (row >= 0 && m_pTable->item(row, kColAction)) {
            m_pTable->item(row, kColAction)->setToolTip(reason);
        }
        const QString title =
                m_trackInfo.value(m_currentUrl).value(QStringLiteral("title")).toString();
        setStatus(tr("No se pudo descargar “%1”: %2").arg(title, reason), true);
    }
    m_currentUrl.clear();
    m_currentAfter = After::Nothing;
    startNextDownload();
}

void DlgSoundCloud::finishDownload(const QString& url, const QString& path) {
    if (url.isEmpty() || path.isEmpty()) {
        return;
    }
    m_pConfig->set(downloadedKey(url), ConfigValue(path));
    const int row = rowForUrl(url);
    if (row >= 0) {
        m_pTable->item(row, kColTrack)->setData(kPathRole, path);
    }
    setUrlState(url, kStateDone, 100);
    // Agrega el tema a la biblioteca de Mixxx
    trackForUrl(url);
    const QString title = m_trackInfo.value(url).value(QStringLiteral("title")).toString();
    setStatus(tr("✓ “%1” listo y en tu biblioteca. Arrastralo a un deck.").arg(title));
    handleAfter(url, m_currentAfter);
    m_currentAfter = After::Nothing;
}

TrackPointer DlgSoundCloud::trackForUrl(const QString& url) {
    const QString path = pathForUrl(url);
    if (path.isEmpty() || !m_pLibrary) {
        return TrackPointer();
    }
    return m_pLibrary->trackCollectionManager()->getOrAddTrack(TrackRef::fromFilePath(path));
}

void DlgSoundCloud::handleAfter(const QString& url, After after) {
    if (after == After::Nothing) {
        return;
    }
    setTargetDeck(url, 0);
    const TrackPointer pTrack = trackForUrl(url);
    if (!pTrack) {
        setStatus(tr("El tema se descargó pero Mixxx no pudo abrirlo."), true);
        return;
    }
    switch (after) {
    case After::AutoDeck:
        emit loadTrack(pTrack);
        break;
    case After::Deck1:
        emit loadTrackToPlayer(pTrack, QStringLiteral("[Channel1]"), false);
        break;
    case After::Deck2:
        emit loadTrackToPlayer(pTrack, QStringLiteral("[Channel2]"), false);
        break;
    case After::Group: {
        const auto target = m_groupTargets.take(url);
        if (!target.first.isEmpty()) {
            emit loadTrackToPlayer(pTrack, target.first, target.second);
        }
        break;
    }
    case After::Nothing:
        break;
    }
}

// ------------------------------------------------------------- utilidades

void DlgSoundCloud::setUrlState(const QString& url, int state, int progress) {
    m_state.insert(url, state);
    m_progress.insert(url, progress);
    const int row = rowForUrl(url);
    if (row < 0) {
        return;
    }
    QTableWidgetItem* pItem = m_pTable->item(row, kColTrack);
    pItem->setData(kStateRole, state);
    pItem->setData(kProgressRole, progress);
    // Repintar la celda de accion
    m_pTable->viewport()->update(m_pTable->visualRect(m_pTable->model()->index(row, kColAction)));
}

void DlgSoundCloud::setStatus(const QString& text, bool error) {
    m_pStatusLabel->setText(text);
    m_pStatusLabel->setStyleSheet(error ? QStringLiteral("color: #ff5d5d;") : QString());
}

void DlgSoundCloud::slotChooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this,
            tr("Dónde guardar las descargas de SoundCloud"),
            downloadDir());
    if (!dir.isEmpty()) {
        m_pConfig->setValue(kDownloadDirKey, dir);
        setStatus(tr("Las descargas se guardan en %1").arg(QDir::toNativeSeparators(dir)));
    }
}

// Estilos en vivo: <carpeta SCDL>/ui/soundcloud.qss se aplica al guardarlo,
// sin recompilar ni reiniciar Mixxx.
QString DlgSoundCloud::styleFile() const {
    return QDir(bridgeDir()).filePath(QStringLiteral("ui/soundcloud.qss"));
}

void DlgSoundCloud::applyStyle() {
    const QString path = styleFile();
    QFile file(path);
    if (!file.exists() && QDir(bridgeDir()).exists()) {
        // Primera vez: dejar una copia editable de los estilos de fabrica
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(kStyleSheet.toUtf8());
            file.close();
        }
    }
    // Estilos de fabrica + los del archivo, que pisan a los de fabrica
    QString style = kStyleSheet;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString custom = QString::fromUtf8(file.readAll());
        file.close();
        if (custom.trimmed() != kStyleSheet.trimmed()) {
            style += QStringLiteral("\n") + custom;
        }
    }
    setStyleSheet(style);
    if (QFileInfo::exists(path) && !m_styleWatcher.files().contains(path)) {
        m_styleWatcher.addPath(path);
    }
}

void DlgSoundCloud::slotStyleFileChanged() {
    // Algunos editores reemplazan el archivo al guardar: esperar un instante
    // y volver a vigilarlo.
    QTimer::singleShot(150, this, [this]() {
        m_styleWatcher.removePaths(m_styleWatcher.files());
        applyStyle();
    });
}

// ------------------------------------------------------------- vistas

void DlgSoundCloud::setGridMode(bool grid) {
    m_gridMode = grid;
    m_pGridButton->setChecked(grid);
    m_pListButton->setChecked(!grid);
    m_pConfig->setValue(kViewModeKey, grid ? QStringLiteral("grid") : QStringLiteral("list"));
    if (!m_pProfileCard->isVisible()) {
        setResultsVisible(true);
    }
}

void DlgSoundCloud::setResultsVisible(bool visible) {
    m_pTable->setVisible(visible && !m_gridMode);
    m_pGrid->setVisible(visible && m_gridMode);
}

void DlgSoundCloud::slotGridClicked(const QModelIndex& index) {
    const QRect cell = m_pGrid->visualRect(index);
    const QPoint pos = m_pGrid->viewport()->mapFromGlobal(QCursor::pos());
    const QTableWidgetItem* pTrack = m_pTable->item(index.row(), kColTrack);
    if (!pTrack) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (gridChipRect(cell, i).adjusted(-3, -3, 3, 3).contains(pos)) {
            loadUrlToGroup(pTrack->data(kUrlRole).toString(), kDeckGroups[i]);
            return;
        }
    }
    if (gridActionRect(cell).adjusted(-3, -3, 3, 3).contains(pos)) {
        const int state = pTrack->data(kStateRole).toInt();
        if (state == kStateNone || state == kStateError) {
            enqueue(index.row(), After::Nothing);
        }
    }
}

void DlgSoundCloud::slotGridDoubleClicked(const QModelIndex& index) {
    const QRect cell = m_pGrid->visualRect(index);
    const QPoint pos = m_pGrid->viewport()->mapFromGlobal(QCursor::pos());
    if (gridActionRect(cell).contains(pos) || gridChipRect(cell, 0).contains(pos) ||
            gridChipRect(cell, 1).contains(pos)) {
        return;
    }
    enqueue(index.row(), After::AutoDeck);
}

void DlgSoundCloud::slotOpenFolder() {
    const QString dir = downloadDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}
