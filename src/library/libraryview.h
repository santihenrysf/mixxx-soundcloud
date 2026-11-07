/// libraryview.h
/// Created 8/28/2009 by RJ Ryan (rryan@mit.edu)
///
/// LibraryView is an abstract interface that all views to be used with the
/// Library widget should support.

#pragma once

#include <QString>

#include "library/trackmodel.h"

class LibraryView {
  public:
    virtual ~LibraryView() {
    }

    virtual void onShow() = 0;
    virtual bool hasFocus() const = 0;
    virtual void setFocus() {
    }
    /// Reimplement if LibraryView should be able to search
    virtual void onSearch(const QString& text) {Q_UNUSED(text);}

    virtual void pasteFromSidebar() {
    }

    virtual void saveCurrentViewState() {
    }
    /// @brief restores current view state.
    /// @return true if restore succeeded
    virtual bool restoreCurrentViewState() {
        return false;
    };

    virtual TrackModel::SortColumnId getColumnIdFromCurrentIndex() {
        return TrackModel::SortColumnId::Invalid;
    }
    /// If applicable, requests that the LibraryView changes the track color of
    /// the selected track. Does nothing otherwise.
    virtual void assignPreviousTrackColor() {
    }
    /// Para vistas que no son una tabla de tracks (ej. SoundCloud):
    /// cargar el elemento seleccionado en un deck desde un controlador.
    /// Devuelve true si la vista lo manejo.
    virtual bool loadSelectedToGroup(const QString& group, bool play) {
        Q_UNUSED(group);
        Q_UNUSED(play);
        return false;
    }
    /// Accion principal sobre el elemento seleccionado ([Library],GoToItem).
    virtual bool activateSelected() {
        return false;
    }
    virtual void assignNextTrackColor() {
    }
};
