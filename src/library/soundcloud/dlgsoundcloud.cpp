#include "library/soundcloud/dlgsoundcloud.h"

#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
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
#include <QListWidget>
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
#include <QSet>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "moc_dlgsoundcloud.cpp"
#include "track/track.h"
#include "track/trackref.h"
#include "widget/wlibrary.h"

namespace {

const QString kConfigGroup = QStringLiteral("[SoundCloud]");
const ConfigKey kBridgeDirKey(kConfigGroup, QStringLiteral("BridgeDir"));
const ConfigKey kDownloadDirKey(kConfigGroup, QStringLiteral("DownloadDir"));
const ConfigKey kProfileKey(kConfigGroup, QStringLiteral("Profile"));

// Datos guardados en la celda del tema
constexpr int kUrlRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;
constexpr int kArtistRole = Qt::UserRole + 3;
constexpr int kStateRole = Qt::UserRole + 4;
constexpr int kProgressRole = Qt::UserRole + 5;
constexpr int kPreviewRole = Qt::UserRole + 6;

constexpr int kColTrack = 0;
constexpr int kColDuration = 1;
constexpr int kColAction = 2;

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

/// Tabla que permite arrastrar los temas ya descargados a los decks.
class SoundCloudTable : public QTableWidget {
  public:
    using QTableWidget::QTableWidget;

  protected:
    QMimeData* mimeData(const QList<QTableWidgetItem*>& items) const override {
        QList<QUrl> urls;
        QSet<int> seenRows;
        for (const QTableWidgetItem* pItem : items) {
            if (!pItem || seenRows.contains(pItem->row())) {
                continue;
            }
            seenRows.insert(pItem->row());
            const QTableWidgetItem* pTrack = item(pItem->row(), kColTrack);
            const QString path = pTrack ? pTrack->data(kPathRole).toString() : QString();
            if (!path.isEmpty()) {
                urls.append(QUrl::fromLocalFile(path));
            }
        }
        if (urls.isEmpty()) {
            return nullptr;
        }
        auto* pMimeData = new QMimeData();
        pMimeData->setUrls(urls);
        return pMimeData;
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
    setObjectName(QStringLiteral("SoundCloudRoot"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(kStyleSheet);

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
    m_pProfileButton = new QPushButton(tr("Mi perfil de SoundCloud"), this);
    prepareButton(m_pProfileButton, "scLink");
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

    m_pTable = new SoundCloudTable(0, 3, pPanel);
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
    m_pTable->horizontalHeader()->setSectionResizeMode(kColTrack, QHeaderView::Stretch);
    m_pTable->horizontalHeader()->setSectionResizeMode(kColDuration, QHeaderView::Fixed);
    m_pTable->horizontalHeader()->setSectionResizeMode(kColAction, QHeaderView::Fixed);
    m_pTable->setColumnWidth(kColDuration, 70);
    m_pTable->setColumnWidth(kColAction, 150);
    m_pTable->setItemDelegateForColumn(kColTrack, new TrackDelegate(m_pTable));
    m_pTable->setItemDelegateForColumn(kColAction, new ActionDelegate(m_pTable));
    pMain->addWidget(m_pTable, 1);

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
    return m_pSearchEdit->hasFocus() || m_pTable->hasFocus() || m_pNav->hasFocus();
}

void DlgSoundCloud::setFocus() {
    if (m_page == Page::Search) {
        m_pSearchEdit->setFocus();
    } else {
        m_pTable->setFocus();
    }
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
    QString value = m_pConfig->getValueString(kProfileKey);
    if (value.isEmpty() && askIfMissing) {
        bool ok = false;
        value = QInputDialog::getText(this,
                tr("Tu SoundCloud"),
                tr("Pegá el link de tu perfil de SoundCloud\n"
                   "(ej. https://soundcloud.com/tu-usuario) o tu usuario:"),
                QLineEdit::Normal,
                QString(),
                &ok)
                        .trimmed();
        if (!ok || value.isEmpty()) {
            return QString();
        }
        m_pConfig->setValue(kProfileKey, value);
    }
    return value;
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
    m_page = static_cast<Page>(index);
    m_pSearchEdit->setVisible(m_page == Page::Search);
    m_pPlaylistCombo->setVisible(m_page == Page::Playlists && m_pPlaylistCombo->count() > 0);
    switch (m_page) {
    case Page::Search:
        setHeader(tr("Buscar"), tr("Encontrá cualquier tema de SoundCloud y llevalo al deck"));
        m_pTable->setRowCount(0);
        m_pSearchEdit->setFocus();
        if (!m_pSearchEdit->text().trimmed().isEmpty()) {
            slotSearch();
        }
        break;
    case Page::Likes: {
        const QString user = profile(true);
        setHeader(tr("Mis likes"), user.isEmpty() ? tr("Configurá tu perfil abajo a la izquierda") : user);
        m_pTable->setRowCount(0);
        if (!user.isEmpty()) {
            runList({QStringLiteral("likes"), user, QStringLiteral("300")},
                    ListKind::Tracks,
                    tr("Cargando tus likes…"));
        }
        break;
    }
    case Page::Playlists: {
        const QString user = profile(true);
        setHeader(tr("Mis playlists"), tr("Elegí una playlist para ver sus temas"));
        m_pTable->setRowCount(0);
        if (!user.isEmpty()) {
            runList({QStringLiteral("playlists"), user},
                    ListKind::Playlists,
                    tr("Cargando tus playlists…"));
        }
        break;
    }
    case Page::Downloads:
        showDownloadsPage();
        break;
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
    bool ok = false;
    const QString value = QInputDialog::getText(this,
            tr("Tu SoundCloud"),
            tr("Link de tu perfil de SoundCloud o tu usuario:"),
            QLineEdit::Normal,
            m_pConfig->getValueString(kProfileKey),
            &ok)
                                  .trimmed();
    if (ok && !value.isEmpty()) {
        m_pConfig->setValue(kProfileKey, value);
        setStatus(tr("Perfil guardado: %1").arg(value));
    }
}

void DlgSoundCloud::setBusy(bool busy) {
    m_pNav->setEnabled(!busy);
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
        } else {
            loadArtwork(url, track.value(QStringLiteral("artwork")).toString());
        }
        ++row;
    }
}

void DlgSoundCloud::loadArtwork(const QString& trackUrl, const QString& artUrl) {
    if (artUrl.isEmpty()) {
        return;
    }
    // Pedir una portada un poco mas grande que la de 100 px por defecto
    QString url = artUrl;
    url.replace(QStringLiteral("-large."), QStringLiteral("-t200x200."));
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
        m_artwork.insert(trackUrl, rounded);
        const int row = rowForUrl(trackUrl);
        if (row >= 0) {
            m_pTable->item(row, kColTrack)->setData(Qt::DecorationRole, rounded);
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
    const QModelIndexList selected = m_pTable->selectionModel()->selectedRows();
    for (const QModelIndex& index : selected) {
        rows.append(index.row());
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

void DlgSoundCloud::slotCellClicked(int row, int column) {
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
    if (column == kColAction) {
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
    enqueue(rows.first(), After::Deck1);
}

void DlgSoundCloud::slotLoadDeck2() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        setStatus(tr("Elegí un tema de la lista."));
        return;
    }
    enqueue(rows.first(), After::Deck2);
}

void DlgSoundCloud::enqueue(int row, After after) {
    const QTableWidgetItem* pItem = m_pTable->item(row, kColTrack);
    if (!pItem) {
        return;
    }
    const QString url = pItem->data(kUrlRole).toString();
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

void DlgSoundCloud::slotOpenFolder() {
    const QString dir = downloadDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}
