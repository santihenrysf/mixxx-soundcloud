#include "library/soundcloud/dlgsoundcloud.h"

#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
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

constexpr int kUrlRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;

constexpr int kColTitle = 0;
constexpr int kColArtist = 1;
constexpr int kColDuration = 2;
constexpr int kColStatus = 3;

const QString kColorOk = QStringLiteral("#3ecf8e");
const QString kColorRun = QStringLiteral("#ff7a33");
const QString kColorErr = QStringLiteral("#ff5d5d");
const QString kColorMuted = QStringLiteral("#8b8b95");

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
            const QTableWidgetItem* pTitle = item(pItem->row(), kColTitle);
            const QString path = pTitle ? pTitle->data(kPathRole).toString() : QString();
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
          m_pListProcess(nullptr),
          m_listKind(ListKind::Tracks),
          m_pDownloadProcess(nullptr),
          m_pNetwork(new QNetworkAccessManager(this)),
          m_listGeneration(0),
          m_currentAfter(After::Nothing) {
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(6, 6, 6, 6);
    pLayout->setSpacing(6);

    // Fila 1: busqueda
    auto* pSearchRow = new QHBoxLayout();
    m_pSearchEdit = new QLineEdit(this);
    m_pSearchEdit->setPlaceholderText(tr("Buscar en SoundCloud (artista, tema...) y Enter"));
    m_pSearchEdit->setClearButtonEnabled(true);
    m_pSearchEdit->setMinimumHeight(30);
    m_pSearchButton = new QPushButton(tr("Buscar"), this);
    m_pSearchButton->setMinimumHeight(30);
    pSearchRow->addWidget(m_pSearchEdit, 1);
    pSearchRow->addWidget(m_pSearchButton);
    pLayout->addLayout(pSearchRow);

    // Fila 2: mi SoundCloud
    auto* pMineRow = new QHBoxLayout();
    m_pLikesButton = new QPushButton(tr("Mis likes"), this);
    m_pPlaylistsButton = new QPushButton(tr("Mis playlists"), this);
    m_pPlaylistCombo = new QComboBox(this);
    m_pPlaylistCombo->setMinimumWidth(260);
    m_pPlaylistCombo->setPlaceholderText(tr("Elegi una playlist..."));
    m_pPlaylistCombo->setVisible(false);
    m_pProfileButton = new QPushButton(tr("Mi perfil..."), this);
    m_pProfileButton->setToolTip(tr("Tu perfil de SoundCloud (link o nombre de usuario)"));
    m_pTitleLabel = new QLabel(this);
    QFont titleFont = m_pTitleLabel->font();
    titleFont.setBold(true);
    m_pTitleLabel->setFont(titleFont);
    pMineRow->addWidget(m_pLikesButton);
    pMineRow->addWidget(m_pPlaylistsButton);
    pMineRow->addWidget(m_pPlaylistCombo);
    pMineRow->addWidget(m_pTitleLabel, 1);
    pMineRow->addWidget(m_pProfileButton);
    pLayout->addLayout(pMineRow);

    // Resultados
    m_pTable = new SoundCloudTable(0, 4, this);
    m_pTable->setHorizontalHeaderLabels(
            {tr("Tema"), tr("Artista"), tr("Duracion"), tr("Estado")});
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->setDragEnabled(true);
    m_pTable->setDragDropMode(QAbstractItemView::DragOnly);
    m_pTable->setIconSize(QSize(36, 36));
    m_pTable->verticalHeader()->setVisible(false);
    m_pTable->verticalHeader()->setDefaultSectionSize(42);
    m_pTable->setShowGrid(false);
    m_pTable->setAlternatingRowColors(true);
    QHeaderView* pHeader = m_pTable->horizontalHeader();
    pHeader->setSectionResizeMode(kColTitle, QHeaderView::Stretch);
    pHeader->setSectionResizeMode(kColArtist, QHeaderView::Interactive);
    pHeader->setSectionResizeMode(kColDuration, QHeaderView::ResizeToContents);
    pHeader->setSectionResizeMode(kColStatus, QHeaderView::Interactive);
    m_pTable->setColumnWidth(kColArtist, 200);
    m_pTable->setColumnWidth(kColStatus, 150);
    pLayout->addWidget(m_pTable, 1);

    // Acciones
    auto* pActions = new QHBoxLayout();
    m_pDownloadButton = new QPushButton(tr("Descargar seleccion"), this);
    m_pDownloadAllButton = new QPushButton(tr("Descargar todo"), this);
    m_pDeck1Button = new QPushButton(tr("Cargar en Deck 1"), this);
    m_pDeck2Button = new QPushButton(tr("Cargar en Deck 2"), this);
    m_pFolderButton = new QPushButton(tr("Carpeta..."), this);
    m_pOpenFolderButton = new QPushButton(tr("Abrir carpeta"), this);
    m_pDeck1Button->setToolTip(tr("Descarga (si hace falta) y carga en el Deck 1"));
    m_pDeck2Button->setToolTip(tr("Descarga (si hace falta) y carga en el Deck 2"));
    m_pFolderButton->setToolTip(tr("Elegir donde se guardan las descargas"));
    pActions->addWidget(m_pDownloadButton);
    pActions->addWidget(m_pDownloadAllButton);
    pActions->addWidget(m_pDeck1Button);
    pActions->addWidget(m_pDeck2Button);
    pActions->addStretch(1);
    pActions->addWidget(m_pFolderButton);
    pActions->addWidget(m_pOpenFolderButton);
    pLayout->addLayout(pActions);

    // Estado
    auto* pStatusRow = new QHBoxLayout();
    m_pStatusLabel = new QLabel(
            tr("Busca arriba o abri tus likes / playlists. Doble click en un "
               "tema: se descarga y se carga en el primer deck libre. Los temas "
               "descargados se pueden arrastrar a cualquier deck."),
            this);
    m_pStatusLabel->setWordWrap(true);
    m_pProgress = new QProgressBar(this);
    m_pProgress->setRange(0, 100);
    m_pProgress->setMaximumWidth(220);
    m_pProgress->setVisible(false);
    pStatusRow->addWidget(m_pStatusLabel, 1);
    pStatusRow->addWidget(m_pProgress);
    pLayout->addLayout(pStatusRow);

    connect(m_pSearchEdit, &QLineEdit::returnPressed, this, &DlgSoundCloud::slotSearch);
    connect(m_pSearchButton, &QPushButton::clicked, this, &DlgSoundCloud::slotSearch);
    connect(m_pLikesButton, &QPushButton::clicked, this, &DlgSoundCloud::slotMyLikes);
    connect(m_pPlaylistsButton, &QPushButton::clicked, this, &DlgSoundCloud::slotMyPlaylists);
    connect(m_pPlaylistCombo,
            QOverload<int>::of(&QComboBox::activated),
            this,
            &DlgSoundCloud::slotPlaylistChosen);
    connect(m_pProfileButton, &QPushButton::clicked, this, &DlgSoundCloud::slotChangeProfile);
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
    m_pSearchEdit->setFocus();
}

bool DlgSoundCloud::hasFocus() const {
    return m_pSearchEdit->hasFocus() || m_pTable->hasFocus();
}

void DlgSoundCloud::setFocus() {
    m_pSearchEdit->setFocus();
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
                tr("Pega el link de tu perfil de SoundCloud\n"
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
    return env;
}

bool DlgSoundCloud::ensureBridge() {
    if (QFileInfo::exists(bridgeScript())) {
        return true;
    }
    setStatus(tr("No encuentro scdl_bridge.py en %1. Elegi la carpeta de SCDL.")
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

// ------------------------------------------------------------- listados

void DlgSoundCloud::slotSearch() {
    const QString query = m_pSearchEdit->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    m_pPlaylistCombo->setVisible(false);
    m_pTitleLabel->setText(tr("Resultados: %1").arg(query));
    runList({QStringLiteral("search"), query, QStringLiteral("40")},
            ListKind::Tracks,
            tr("Buscando \"%1\"...").arg(query));
}

void DlgSoundCloud::slotMyLikes() {
    const QString user = profile(true);
    if (user.isEmpty()) {
        return;
    }
    m_pPlaylistCombo->setVisible(false);
    m_pTitleLabel->setText(tr("Mis likes"));
    runList({QStringLiteral("likes"), user, QStringLiteral("300")},
            ListKind::Tracks,
            tr("Cargando tus likes..."));
}

void DlgSoundCloud::slotMyPlaylists() {
    const QString user = profile(true);
    if (user.isEmpty()) {
        return;
    }
    runList({QStringLiteral("playlists"), user},
            ListKind::Playlists,
            tr("Cargando tus playlists..."));
}

void DlgSoundCloud::slotPlaylistChosen(int index) {
    const QString url = m_pPlaylistCombo->itemData(index).toString();
    if (url.isEmpty()) {
        return;
    }
    m_pTitleLabel->setText(m_pPlaylistCombo->itemText(index));
    runList({QStringLiteral("playlist"), url},
            ListKind::Tracks,
            tr("Cargando playlist..."));
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
    m_pSearchButton->setEnabled(!busy);
    m_pLikesButton->setEnabled(!busy);
    m_pPlaylistsButton->setEnabled(!busy);
}

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
            setStatus(tr("No pude iniciar Python (%1). Abri SCDL.vbs una vez "
                         "para que se instale.")
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
    fillTracks(result.value(QStringLiteral("results")).toArray());
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
        setStatus(tr("No encontre playlists publicas en tu perfil."));
    } else {
        setStatus(tr("%1 playlists. Elegi una en la lista.").arg(playlists.size()));
        m_pPlaylistCombo->showPopup();
    }
}

void DlgSoundCloud::fillTracks(const QJsonArray& results) {
    ++m_listGeneration;
    m_pTable->setRowCount(0);
    m_pTable->setRowCount(static_cast<int>(results.size()));
    int row = 0;
    for (const QJsonValue& value : results) {
        const QJsonObject track = value.toObject();
        const QString url = track.value(QStringLiteral("url")).toString();
        const QString title = track.value(QStringLiteral("title")).toString();
        m_titles.insert(url, title);

        auto* pTitle = new QTableWidgetItem(title);
        pTitle->setData(kUrlRole, url);
        pTitle->setToolTip(url);
        QString artist = track.value(QStringLiteral("artist")).toString();
        if (track.value(QStringLiteral("preview")).toBool()) {
            artist += tr("  (solo 30 s)");
        }
        auto* pDuration = new QTableWidgetItem(formatDuration(static_cast<qint64>(
                track.value(QStringLiteral("duration_ms")).toDouble())));
        pDuration->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_pTable->setItem(row, kColTitle, pTitle);
        m_pTable->setItem(row, kColArtist, new QTableWidgetItem(artist));
        m_pTable->setItem(row, kColDuration, pDuration);
        m_pTable->setItem(row, kColStatus, new QTableWidgetItem());

        const QString path = pathForUrl(url);
        if (!path.isEmpty()) {
            pTitle->setData(kPathRole, path);
            setUrlStatus(url, tr("En biblioteca"), kColorOk);
        }
        loadArtwork(row, track.value(QStringLiteral("artwork")).toString());
        ++row;
    }
    // Estado de la cola actual
    if (!m_currentUrl.isEmpty()) {
        setUrlStatus(m_currentUrl, tr("Descargando..."), kColorRun);
    }
    for (const auto& entry : std::as_const(m_queue)) {
        setUrlStatus(entry.first, tr("En cola"), kColorMuted);
    }
    setStatus(results.isEmpty()
                    ? tr("Sin resultados.")
                    : tr("%1 temas. Doble click: descargar y cargar en un deck. "
                         "Arrastra los ya descargados a un deck.")
                              .arg(results.size()));
}

void DlgSoundCloud::loadArtwork(int row, const QString& url) {
    if (url.isEmpty()) {
        return;
    }
    const int generation = m_listGeneration;
    QNetworkReply* pReply = m_pNetwork->get(QNetworkRequest(QUrl(url)));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, row, generation]() {
        pReply->deleteLater();
        if (generation != m_listGeneration || pReply->error() != QNetworkReply::NoError) {
            return;
        }
        QPixmap pixmap;
        if (pixmap.loadFromData(pReply->readAll())) {
            QTableWidgetItem* pItem = m_pTable->item(row, kColTitle);
            if (pItem) {
                pItem->setIcon(QIcon(pixmap.scaled(72,
                        72,
                        Qt::KeepAspectRatioByExpanding,
                        Qt::SmoothTransformation)));
            }
        }
    });
}

// ------------------------------------------------------------- descargas

int DlgSoundCloud::rowForUrl(const QString& url) const {
    for (int row = 0; row < m_pTable->rowCount(); ++row) {
        const QTableWidgetItem* pItem = m_pTable->item(row, kColTitle);
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

void DlgSoundCloud::slotCellDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    enqueue(row, After::AutoDeck);
}

void DlgSoundCloud::slotDownloadSelected() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) {
        setStatus(tr("Selecciona uno o mas temas de la lista."));
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
    if (!rows.isEmpty()) {
        enqueue(rows.first(), After::Deck1);
    }
}

void DlgSoundCloud::slotLoadDeck2() {
    const QList<int> rows = selectedRows();
    if (!rows.isEmpty()) {
        enqueue(rows.first(), After::Deck2);
    }
}

void DlgSoundCloud::enqueue(int row, After after) {
    const QTableWidgetItem* pItem = m_pTable->item(row, kColTitle);
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
    setUrlStatus(url, tr("En cola"), kColorMuted);
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

    setUrlStatus(m_currentUrl, tr("Descargando 0%"), kColorRun);
    m_pProgress->setValue(0);
    m_pProgress->setVisible(true);
    const int pending = static_cast<int>(m_queue.size());
    setStatus(pending > 0
                    ? tr("Descargando: %1  (%2 en cola)")
                              .arg(m_titles.value(m_currentUrl, m_currentUrl))
                              .arg(pending)
                    : tr("Descargando: %1").arg(m_titles.value(m_currentUrl, m_currentUrl)));
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
            setUrlStatus(m_currentUrl, tr("Descargando %1%").arg(pct), kColorRun);
            QString status = tr("Descargando: %1  -  %2%")
                                     .arg(m_titles.value(m_currentUrl, m_currentUrl))
                                     .arg(pct);
            const QString speed = event.value(QStringLiteral("speed")).toString();
            const QString eta = event.value(QStringLiteral("eta")).toString();
            if (!speed.isEmpty()) {
                status += QStringLiteral("  -  ") + speed;
            }
            if (!eta.isEmpty()) {
                status += tr("  -  quedan %1").arg(eta);
            }
            if (!m_queue.isEmpty()) {
                status += tr("  (%1 en cola)").arg(m_queue.size());
            }
            setStatus(status);
        } else if (event.contains(QStringLiteral("status"))) {
            setUrlStatus(m_currentUrl, tr("Procesando..."), kColorRun);
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
            reason = tr("el proceso termino con codigo %1").arg(exitCode);
        }
        setUrlStatus(m_currentUrl, tr("Error"), kColorErr);
        const int row = rowForUrl(m_currentUrl);
        if (row >= 0 && m_pTable->item(row, kColStatus)) {
            m_pTable->item(row, kColStatus)->setToolTip(reason);
        }
        setStatus(tr("No se pudo descargar %1: %2")
                          .arg(m_titles.value(m_currentUrl, m_currentUrl), reason),
                true);
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
        m_pTable->item(row, kColTitle)->setData(kPathRole, path);
    }
    // Agrega el tema a la biblioteca de Mixxx
    const TrackPointer pTrack = trackForUrl(url);
    setUrlStatus(url, pTrack ? tr("En biblioteca") : tr("Descargado"), kColorOk);
    setStatus(tr("Listo: %1. Arrastralo a un deck o usa los botones.")
                      .arg(m_titles.value(url, url)));
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
        setStatus(tr("El tema se descargo pero Mixxx no pudo abrirlo."), true);
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

void DlgSoundCloud::setUrlStatus(const QString& url, const QString& text, const QString& color) {
    const int row = rowForUrl(url);
    if (row < 0) {
        return;
    }
    QTableWidgetItem* pItem = m_pTable->item(row, kColStatus);
    if (!pItem) {
        return;
    }
    pItem->setText(text);
    pItem->setForeground(QColor(color));
}

void DlgSoundCloud::setStatus(const QString& text, bool error) {
    m_pStatusLabel->setText(text);
    m_pStatusLabel->setStyleSheet(
            error ? QStringLiteral("color: %1;").arg(kColorErr) : QString());
}

void DlgSoundCloud::slotChooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this,
            tr("Donde guardar las descargas de SoundCloud"),
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
