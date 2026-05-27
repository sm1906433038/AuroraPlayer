// PlaylistPanel.h
//
// A dockable side panel exposing a play queue:
//   * Drag-drop to reorder
//   * External file / folder drag-in
//   * Double-click → play that item
//   * Toolbar: 添加 / 移除 / 清空 / 上移 / 下移 / 导入 m3u / 导出 m3u
//   * Persistable to QSettings as a QStringList
//
// The panel does NOT touch the player directly — host connects:
//   playRequested(path) → MainWindow::openPath(path)
//   stops emitting events for the host's own programmatic mutations.

#pragma once

#include <QDockWidget>
#include <QStringList>

class QListWidget;
class QListWidgetItem;

namespace promp {

class PlaylistPanel : public QDockWidget {
    Q_OBJECT
public:
    explicit PlaylistPanel(QWidget* parent = nullptr);

    QStringList items() const;
    void        setItems(const QStringList& paths);

    /// Index of currently-highlighted "now playing" row. -1 if none.
    [[nodiscard]] int currentRow() const;
    void              setCurrentRow(int row);

    /// Path at row, or QString() if out of range.
    [[nodiscard]] QString pathAt(int row) const;
    [[nodiscard]] int     count()        const;

public slots:
    void addPath(const QString& path);
    void addPaths(const QStringList& paths);
    void removeSelected();
    void clearAll();
    void moveSelectedUp();
    void moveSelectedDown();
    void importM3u();
    void exportM3u();
    void playNext();
    void playPrevious();
    /// Called by the host whenever a real load happens — keeps highlight synced.
    void notePlaying(const QString& path);

signals:
    void playRequested(const QString& path);

private slots:
    void onItemActivated(QListWidgetItem* item);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildUi();
    void rebuildItems();
    static QString labelFor(const QString& path);

    QListWidget* m_list = nullptr;
    QStringList  m_paths;
    int          m_currentRow = -1;
};

} // namespace promp
