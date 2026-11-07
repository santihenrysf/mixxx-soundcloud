#pragma once

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPixmap>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "library/libraryview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class Library;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;
class QTableWidget;
class WLibrary;

/// Seccion "SoundCloud" de la biblioteca de Mixxx:
/// buscar, ver tus likes y playlists, descargar y cargar en los decks.
/// Habla con scdl_bridge.py (Python) mediante JSON por stdout.
class DlgSoundCloud : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgSoundCloud(WLibrary* parent, UserSettingsPointer pConfig, Library* pLibrary);
    ~DlgSoundCloud() override;

    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;
    bool loadSelectedToGroup(const QString& group, bool play) override;
    bool activateSelected() override;

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void slotNavChanged(int index);
    void slotSearch();
    void slotPlaylistChosen(int index);
    void slotChangeProfile();
    void slotListFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotDownloadOutput();
    void slotDownloadFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotCellClicked(int row, int column);
    void slotCellDoubleClicked(int row, int column);
    void slotDownloadSelected();
    void slotDownloadAll();
    void slotLoadDeck1();
    void slotLoadDeck2();
    void slotChooseFolder();
    void slotOpenFolder();
    void slotStyleFileChanged();

  private:
    enum class After {
        Nothing,
        AutoDeck,
        Deck1,
        Deck2,
        Group, // deck/sampler pedido por un controlador (ver m_groupTargets)
    };
    enum class ListKind {
        Tracks,
        Playlists,
    };
    enum class Page {
        Search,
        Likes,
        Playlists,
        Downloads,
    };

    QString bridgeDir() const;
    QString pythonPath() const;
    QString bridgeScript() const;
    QString downloadDir() const;
    QString profile(bool askIfMissing);
    bool ensureBridge();
    QProcessEnvironment processEnvironment() const;

    void setHeader(const QString& title, const QString& subtitle);
    void runList(const QStringList& args, ListKind kind, const QString& busyText);
    void fillTracks(const QJsonArray& results);
    void fillPlaylists(const QJsonArray& playlists);
    void showDownloadsPage();

    int rowForUrl(const QString& url) const;
    QString pathForUrl(const QString& url) const;
    QList<int> selectedRows() const;
    void enqueue(int row, After after);
    void startNextDownload();
    void finishDownload(const QString& url, const QString& path);
    void setUrlState(const QString& url, int state, int progress = 0);
    void setStatus(const QString& text, bool error = false);
    void handleAfter(const QString& url, After after);
    TrackPointer trackForUrl(const QString& url);
    void loadArtwork(const QString& trackUrl, const QString& artUrl);
    void setBusy(bool busy);
    QString styleFile() const;
    void applyStyle();

    const UserSettingsPointer m_pConfig;
    Library* const m_pLibrary;

    QListWidget* m_pNav;
    QLabel* m_pTitle;
    QLabel* m_pSubtitle;
    QLineEdit* m_pSearchEdit;
    QComboBox* m_pPlaylistCombo;
    QTableWidget* m_pTable;
    QPushButton* m_pDeck1Button;
    QPushButton* m_pDeck2Button;
    QPushButton* m_pDownloadButton;
    QPushButton* m_pDownloadAllButton;
    QPushButton* m_pFolderButton;
    QPushButton* m_pOpenFolderButton;
    QPushButton* m_pProfileButton;
    QLabel* m_pStatusLabel;
    QProgressBar* m_pProgress;

    Page m_page;
    QProcess* m_pListProcess;
    ListKind m_listKind;
    QProcess* m_pDownloadProcess;
    QByteArray m_downloadBuffer;
    QString m_lastError;
    QNetworkAccessManager* m_pNetwork;
    QFileSystemWatcher m_styleWatcher;

    QHash<QString, QJsonObject> m_trackInfo; // url -> datos del tema
    QHash<QString, QPixmap> m_artwork;       // url del tema -> portada
    QHash<QString, int> m_state;             // url -> estado de descarga
    QHash<QString, int> m_progress;          // url -> % descargado
    QHash<QString, QPair<QString, bool>> m_groupTargets; // url -> (grupo, play)
    QStringList m_sessionDownloads;          // orden de la pagina Descargas
    QList<QPair<QString, After>> m_queue;
    QString m_currentUrl;
    After m_currentAfter;
};
