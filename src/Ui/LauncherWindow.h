#pragma once

#include "Core/SearchEngine.h"

#include <QPoint>
#include <QWidget>

class QFrame;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QAction;
class QTimer;
class ResultModel;
class SearchService;

/**
 * 唤起的搜索面板：无边框、置顶、屏幕居中靠上弹出。
 * 输入即搜，有候选时才展开列表；↑↓ 选择，Enter 运行，Ctrl+Enter 定位，Shift+Enter 提权运行。
 */
class LauncherWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LauncherWindow(SearchService *service, int maxResults, QWidget *parent = nullptr);
    ~LauncherWindow() override;

    void popup();
    void dismiss();
    void refreshResults();

    void setMaxResults(int maxResults);
    void setHideOnFocusLost(bool on);
    void setStatusText(const QString &text);

    /** 离屏预览用：直接设置输入框内容并触发一次检索（不改变窗口可见性）。 */
    void setQueryForPreview(const QString &query);

    /** 等待当前输入对应的检索结果落到界面上（预览/自检没有常驻事件循环时使用）。 */
    bool waitForSearch(int timeoutMs = 3000);

    /** 构造结果项右键菜单（自检也用它验证菜单项）。调用方负责 delete。 */
    QMenu *createResultMenu(const SearchResult &result,
                            QAction **adminAction = nullptr,
                            QAction **revealAction = nullptr,
                            QAction **copyAction = nullptr) const;

signals:
    void launchRequested(const ExecutableRecord &record, bool asAdmin);
    void revealRequested(const ExecutableRecord &record);
    void settingsRequested();
    /** 一次匹配当前输入的结果已写入列表（预览/自检等待用）。 */
    void searchApplied();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void applyResults(quint64 sequence, const QString &query,
                      const QVector<SearchResult> &results, qint64 elapsedMs);

private:
    void buildUi();
    void scheduleSearch();
    void performSearch();
    void moveSelection(int delta);
    void activateCurrent(bool reveal, bool asAdmin);
    void updateGeometryForResults(int rowCount, bool emptyQuery);
    void showContextMenu(const QPoint &position);

    SearchService *m_service = nullptr;
    int m_maxResults = 12;
    bool m_hideOnFocusLost = true;
    /** 右键菜单打开期间禁止失焦自动隐藏，避免嵌套事件循环里模型被清空。 */
    bool m_contextMenuOpen = false;
    /** 最近一次发出的检索序号；只有它的结果才允许写入列表。 */
    quint64 m_pendingSequence = 0;
    quint64 m_appliedSequence = 0;

    QFrame *m_card = nullptr;
    QLineEdit *m_edit = nullptr;
    QFrame *m_separator = nullptr;
    QListView *m_list = nullptr;
    ResultModel *m_model = nullptr;
    QWidget *m_footer = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_hints = nullptr;
    QTimer *m_debounce = nullptr;
};
