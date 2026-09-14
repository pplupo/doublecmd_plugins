#pragma once

#include <QStringList>
#include <QTreeView>

/// QTreeView that can drag its selected members out as real files.
///
/// Members do not exist on disk, so a drag has to produce them. The view asks
/// its owner to materialise the selection through `extractor`, then hands the
/// resulting paths to the drop target as text/uri-list.
///
/// Note this is the eager form: files are extracted when the drag begins, not
/// when it is dropped. The lazy form is XDS (XdndDirectSave0), which is an
/// X11 XDND protocol extension with no Wayland equivalent — and this
/// repository's Qt base targets Wayland, so implementing it would mean
/// carrying a code path that does not run on the primary target. Extraction
/// still happens no earlier than the drag: nothing is written just by
/// selecting rows.
class ArchiveTreeView : public QTreeView {
    Q_OBJECT
public:
    explicit ArchiveTreeView(QWidget *parent = nullptr);

    /// Callback that extracts the current selection and returns the paths
    /// written, or an empty list to abandon the drag.
    using Materialiser = std::function<QStringList()>;
    void setMaterialiser(Materialiser materialiser);

protected:
    void startDrag(Qt::DropActions supportedActions) override;

private:
    Materialiser m_materialise;
};
