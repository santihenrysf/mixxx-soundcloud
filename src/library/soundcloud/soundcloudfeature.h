#pragma once

#include <QPointer>
#include <QVariant>

#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class DlgSoundCloud;

/// Seccion "SoundCloud" en la barra lateral de la biblioteca.
class SoundCloudFeature final : public LibraryFeature {
    Q_OBJECT
  public:
    SoundCloudFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~SoundCloudFeature() override = default;

    QVariant title() override;
    void bindLibraryWidget(WLibrary* pLibraryWidget,
            KeyboardEventFilter* pKeyboard) override;
    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;

  private:
    parented_ptr<TreeItemModel> m_pSidebarModel;
    QPointer<DlgSoundCloud> m_pView;
};
