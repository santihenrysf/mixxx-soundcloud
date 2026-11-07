#include "library/soundcloud/soundcloudfeature.h"

#include "library/soundcloud/dlgsoundcloud.h"
#include "moc_soundcloudfeature.cpp"
#include "widget/wlibrary.h"

namespace {

const QString kViewName = QStringLiteral("SoundCloud");

} // anonymous namespace

SoundCloudFeature::SoundCloudFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("soundcloud")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    // Primera vez: interfaz mas limpia, sin la fila de samplers ni la de
    // microfonos/auxiliares (se pueden volver a mostrar desde el menu View).
    const ConfigKey firstRunKey(QStringLiteral("[SoundCloud]"), QStringLiteral("CleanUiApplied"));
    if (!m_pConfig->getValue(firstRunKey, false)) {
        m_pConfig->setValue(ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("show_samplers")), 0);
        m_pConfig->setValue(ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("show_microphones")), 0);
        m_pConfig->setValue(firstRunKey, true);
    }
}

QVariant SoundCloudFeature::title() {
    return QVariant(QStringLiteral("SoundCloud"));
}

TreeItemModel* SoundCloudFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void SoundCloudFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* pKeyboard) {
    Q_UNUSED(pKeyboard);
    // La vista la destruye WLibrary
    m_pView = new DlgSoundCloud(pLibraryWidget, m_pConfig, m_pLibrary);
    pLibraryWidget->registerView(kViewName, m_pView);
    connect(m_pView,
            &DlgSoundCloud::loadTrack,
            this,
            &SoundCloudFeature::loadTrack);
    connect(m_pView,
            &DlgSoundCloud::loadTrackToPlayer,
            this,
            &SoundCloudFeature::loadTrackToPlayer);
}

void SoundCloudFeature::activate() {
    emit switchToView(kViewName);
    emit disableSearch();
    emit enableCoverArtDisplay(false);
    if (m_pView) {
        m_pView->setFocus();
    }
}
