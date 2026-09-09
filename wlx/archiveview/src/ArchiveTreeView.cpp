#include "ArchiveTreeView.h"

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QUrl>

ArchiveTreeView::ArchiveTreeView(QWidget *parent)
    : QTreeView(parent)
{
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
}

void ArchiveTreeView::setMaterialiser(Materialiser materialiser)
{
    m_materialise = std::move(materialiser);
}

void ArchiveTreeView::startDrag(Qt::DropActions supportedActions)
{
    Q_UNUSED(supportedActions);

    if (!m_materialise)
        return;

    const QStringList files = m_materialise();
    if (files.isEmpty())
        return;   // nothing extractable, or the user cancelled

    QList<QUrl> urls;
    urls.reserve(files.size());
    for (const QString &file : files)
        urls.append(QUrl::fromLocalFile(file));

    auto *mime = new QMimeData;
    mime->setUrls(urls);

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    // Copy only: a move would imply the plugin can delete from the archive,
    // and this is a read-only viewer.
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}
