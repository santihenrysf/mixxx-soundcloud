#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QPair>
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
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;
class QTableWidget;
class WLibrary;

/// Vista de la seccion "SoundCloud" de la biblioteca de Mixxx:
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

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void slotSearch();
    void slotMyLikes();
    void slotMyPlaylists();
    void slotPlaylistChosen(int index);
    void slotChangeProfile();
    void slotListFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotDownloadOutput();
    void slotDownloadFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void slotCellDoubleClicked(int row, int column);
    void slotDownloadSelected();
    void slotDownloadAll();
    void slotLoadDeck1();
    void slotLoadDeck2();
    void slotChooseFolder();
    void slotOpenFolder();

  private:
    enum class After {
        Nothing,
        AutoDeck,
        Deck1,
        Deck2,
    };
    enum class ListKind {
        Tracks,
        Playlists,
    };

    QString bridgeDir() const;
    QString pythonPath() const;
    QString bridgeScript() const;
    QString downloadDir() const;
    QString profile(bool askIfMissing);
    bool ensureBridge();
    QProcessEnvironment processEnvironment() const;

    void runList(const QStringList& args, ListKind kind, const QString& busyText);
    void fillTracks(const QJsonArray& results);
    void fillPlaylists(const QJsonArray& playlists);

    int rowForUrl(const QString& url) const;
    QString pathForUrl(const QString& url) const;
    QList<int> selectedRows() const;
    void enqueue(int row, After after);
    void startNextDownload();
    void finishDownload(const QString& url, const QString& path);
    void setUrlStatus(const QString& url, const QString& text, const QString& color);
    void setStatus(const QString& text, bool error = false);
    void handleAfter(const QString& url, After after);
    TrackPointer trackForUrl(const QString& url);
    void loadArtwork(int row, const QString& url);
    void setBusy(bool busy);

    const UserSettingsPointer m_pConfig;
    Library* const m_pLibrary;

    QLineEdit* m_pSearchEdit;
    QPushButton* m_pSearchButton;
    QPushButton* m_pLikesButton;
    QPushButton* m_pPlaylistsButton;
    QComboBox* m_pPlaylistCombo;
    QPushButton* m_pProfileButton;
    QLabel* m_pTitleLabel;
    QTableWidget* m_pTable;
    QPushButton* m_pDownloadButton;
    QPushButton* m_pDownloadAllButton;
    QPushButton* m_pDeck1Button;
    QPushButton* m_pDeck2Button;
    QPushButton* m_pFolderButton;
    QPushButton* m_pOpenFolderButton;
    QLabel* m_pStatusLabel;
    QProgressBar* m_pProgress;

    QProcess* m_pListProcess;
    ListKind m_listKind;
    QProcess* m_pDownloadProcess;
    QByteArray m_downloadBuffer;
    QString m_lastError;
    QNetworkAccessManager* m_pNetwork;
    int m_listGeneration;

    QHash<QString, QString> m_titles; // url -> titulo (para la cola)
    QList<QPair<QString, After>> m_queue;
    QString m_currentUrl;
    After m_currentAfter;
};
