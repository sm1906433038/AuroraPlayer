#include "PlaylistPanel.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QListWidget>
#include <QMimeData>
#include <QStyle>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace promp {

PlaylistPanel::PlaylistPanel(QWidget* parent)
    : QDockWidget(tr("播放列表"), parent) {
    setObjectName("PlaylistPanel");
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable
              | QDockWidget::DockWidgetMovable
              | QDockWidget::DockWidgetFloatable);
    buildUi();
}

void PlaylistPanel::buildUi() {
    auto* host = new QWidget(this);
    auto* lay  = new QVBoxLayout(host);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    auto* tb = new QToolBar(this);
    tb->setIconSize({16, 16});

    auto addAct = [&](QStyle::StandardPixmap icon, const QString& text, auto slot) {
        auto* a = tb->addAction(style()->standardIcon(icon), text, this, slot);
        a->setToolTip(text);
        return a;
    };
    addAct(QStyle::SP_DialogOpenButton,  tr("添加文件..."),    [this]() {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("添加到播放列表"),
            QString(),
            tr("视频文件 (*.mp4 *.mkv *.webm *.mov *.avi *.flv *.ts *.m2ts *.wmv *.vob);;"
               "音频文件 (*.mp3 *.flac *.aac *.ogg *.opus *.wav *.m4a);;"
               "所有文件 (*.*)"));
        addPaths(files);
    });
    addAct(QStyle::SP_TrashIcon,          tr("移除所选"),       &PlaylistPanel::removeSelected);
    addAct(QStyle::SP_DialogResetButton,  tr("清空"),          &PlaylistPanel::clearAll);
    tb->addSeparator();
    addAct(QStyle::SP_ArrowUp,            tr("上移"),          &PlaylistPanel::moveSelectedUp);
    addAct(QStyle::SP_ArrowDown,          tr("下移"),          &PlaylistPanel::moveSelectedDown);
    tb->addSeparator();
    addAct(QStyle::SP_FileIcon,           tr("导入 m3u..."),   &PlaylistPanel::importM3u);
    addAct(QStyle::SP_DialogSaveButton,   tr("导出 m3u..."),   &PlaylistPanel::exportM3u);
    lay->addWidget(tb);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragDropMode(QAbstractItemView::DragDrop);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setMovement(QListView::Snap);
    m_list->viewport()->setAcceptDrops(true);
    m_list->setUniformItemSizes(true);
    m_list->installEventFilter(this);
    lay->addWidget(m_list, /*stretch*/ 1);

    connect(m_list, &QListWidget::itemActivated,
            this,   &PlaylistPanel::onItemActivated);
    // Re-sync m_paths whenever the list rearranges via drag-drop.
    connect(m_list->model(), &QAbstractItemModel::rowsMoved,
            this, [this]() {
                m_paths.clear();
                for (int i = 0; i < m_list->count(); ++i) {
                    m_paths.append(m_list->item(i)->data(Qt::UserRole).toString());
                }
            });

    setWidget(host);
}

// ---------- public API ------------------------------------------------------

QStringList PlaylistPanel::items() const { return m_paths; }

void PlaylistPanel::setItems(const QStringList& paths) {
    m_paths = paths;
    rebuildItems();
}

int PlaylistPanel::currentRow() const { return m_currentRow; }
int PlaylistPanel::count()      const { return m_paths.size(); }

QString PlaylistPanel::pathAt(int row) const {
    return (row >= 0 && row < m_paths.size()) ? m_paths[row] : QString();
}

void PlaylistPanel::setCurrentRow(int row) {
    m_currentRow = row;
    for (int i = 0; i < m_list->count(); ++i) {
        QFont f = m_list->item(i)->font();
        f.setBold(i == row);
        m_list->item(i)->setFont(f);
        m_list->item(i)->setForeground(i == row
            ? QBrush(QColor(120, 180, 255))
            : QBrush());
    }
}

void PlaylistPanel::addPath(const QString& path) {
    if (path.isEmpty() || m_paths.contains(path)) return;
    m_paths.append(path);
    auto* it = new QListWidgetItem(labelFor(path), m_list);
    it->setData(Qt::UserRole, path);
    it->setToolTip(path);
}

void PlaylistPanel::addPaths(const QStringList& paths) {
    for (const QString& p : paths) addPath(p);
}

void PlaylistPanel::removeSelected() {
    const auto rows = m_list->selectionModel()->selectedRows();
    QList<int> idxs;
    for (const auto& mi : rows) idxs.append(mi.row());
    std::sort(idxs.begin(), idxs.end(), std::greater<int>());
    for (int r : idxs) {
        delete m_list->takeItem(r);
        m_paths.removeAt(r);
        if (r == m_currentRow)     m_currentRow = -1;
        else if (r <  m_currentRow) --m_currentRow;
    }
}

void PlaylistPanel::clearAll() {
    m_paths.clear();
    m_list->clear();
    m_currentRow = -1;
}

void PlaylistPanel::moveSelectedUp() {
    auto rows = m_list->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex& a, const QModelIndex& b){ return a.row() < b.row(); });
    for (const auto& mi : rows) {
        const int r = mi.row();
        if (r <= 0) continue;
        m_paths.swapItemsAt(r, r - 1);
        QListWidgetItem* it = m_list->takeItem(r);
        m_list->insertItem(r - 1, it);
        m_list->setCurrentItem(it);
    }
}

void PlaylistPanel::moveSelectedDown() {
    auto rows = m_list->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex& a, const QModelIndex& b){ return a.row() > b.row(); });
    for (const auto& mi : rows) {
        const int r = mi.row();
        if (r >= m_list->count() - 1) continue;
        m_paths.swapItemsAt(r, r + 1);
        QListWidgetItem* it = m_list->takeItem(r);
        m_list->insertItem(r + 1, it);
        m_list->setCurrentItem(it);
    }
}

void PlaylistPanel::importM3u() {
    const QString f = QFileDialog::getOpenFileName(this, tr("导入播放列表"), QString(),
        tr("M3U 播放列表 (*.m3u *.m3u8);;所有文件 (*.*)"));
    if (f.isEmpty()) return;
    QFile fd(f);
    if (!fd.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream ts(&fd);
    ts.setEncoding(QStringConverter::Utf8);
    const QDir base = QFileInfo(f).absoluteDir();
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (QFileInfo(line).isRelative() && !line.contains("://"))
            line = base.absoluteFilePath(line);
        addPath(line);
    }
}

void PlaylistPanel::exportM3u() {
    if (m_paths.isEmpty()) return;
    const QString f = QFileDialog::getSaveFileName(this, tr("导出播放列表"),
        QStringLiteral("playlist.m3u8"),
        tr("M3U 播放列表 (*.m3u8 *.m3u)"));
    if (f.isEmpty()) return;
    QFile fd(f);
    if (!fd.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    QTextStream ts(&fd);
    ts.setEncoding(QStringConverter::Utf8);
    ts << "#EXTM3U\n";
    for (const QString& p : m_paths) {
        ts << "#EXTINF:-1," << QFileInfo(p).completeBaseName() << '\n'
           << p << '\n';
    }
}

void PlaylistPanel::playNext() {
    if (m_paths.isEmpty()) return;
    const int next = (m_currentRow + 1) % m_paths.size();
    emit playRequested(m_paths[next]);
}

void PlaylistPanel::playPrevious() {
    if (m_paths.isEmpty()) return;
    int prev = m_currentRow - 1;
    if (prev < 0) prev = m_paths.size() - 1;
    emit playRequested(m_paths[prev]);
}

void PlaylistPanel::notePlaying(const QString& path) {
    int idx = m_paths.indexOf(path);
    if (idx < 0) {
        addPath(path);
        idx = m_paths.size() - 1;
    }
    setCurrentRow(idx);
    if (idx >= 0 && idx < m_list->count()) {
        m_list->setCurrentRow(idx);
        m_list->scrollToItem(m_list->item(idx));
    }
}

void PlaylistPanel::onItemActivated(QListWidgetItem* item) {
    if (!item) return;
    emit playRequested(item->data(Qt::UserRole).toString());
}

// ---------- helpers ---------------------------------------------------------

void PlaylistPanel::rebuildItems() {
    m_list->clear();
    for (const QString& p : m_paths) {
        auto* it = new QListWidgetItem(labelFor(p), m_list);
        it->setData(Qt::UserRole, p);
        it->setToolTip(p);
    }
}

QString PlaylistPanel::labelFor(const QString& path) {
    if (path.contains("://")) return path;          // remote URL
    return QFileInfo(path).fileName();
}

// ---------- DnD + keys ------------------------------------------------------

bool PlaylistPanel::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == m_list) {
        switch (ev->type()) {
            case QEvent::DragEnter: {
                auto* de = static_cast<QDragEnterEvent*>(ev);
                if (de->mimeData()->hasUrls()) { de->acceptProposedAction(); return true; }
                break;
            }
            case QEvent::DragMove: {
                auto* de = static_cast<QDragMoveEvent*>(ev);
                if (de->mimeData()->hasUrls()) { de->acceptProposedAction(); return true; }
                break;
            }
            case QEvent::Drop: {
                auto* de = static_cast<QDropEvent*>(ev);
                if (de->mimeData()->hasUrls()) {
                    QStringList paths;
                    for (const QUrl& u : de->mimeData()->urls()) {
                        paths.append(u.isLocalFile() ? u.toLocalFile() : u.toString());
                    }
                    addPaths(paths);
                    de->acceptProposedAction();
                    return true;
                }
                break;
            }
            case QEvent::KeyPress: {
                auto* ke = static_cast<QKeyEvent*>(ev);
                if (ke->key() == Qt::Key_Delete) { removeSelected(); return true; }
                break;
            }
            default: break;
        }
    }
    return QDockWidget::eventFilter(obj, ev);
}

} // namespace promp
