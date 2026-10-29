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

/**
 * 唤起的搜索面板：无边框、置顶、屏幕居中靠上弹出。
 * 输入即搜，有候选时才展开列表；↑↓ 选择，Enter 运行，Ctrl+Enter 定位，Shift+Enter 提权运行。
 */
class LauncherWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LauncherWindow(SearchEngine *engine, int maxResults, QWidget *parent = nullptr);
    ~LauncherWindow() override;

    void popup();
    void dismiss();
    void refreshResults();

    void setMaxResults(int maxResults);
    void setHideOnFocusLost(bool on);
    void setStatusText(const QString &text);

    /** 离屏预览用：直接设置输入框内容并触发一次检索（不改变窗口可见性）。 */
    void setQueryForPreview(const QString &query);

    /** 构造结果项右键菜单（自检也用它验证菜单项）。调用方负责 delete。 */
    QMenu *createResultMenu(const SearchResult &result,
                            QAction **adminAction = nullptr,
                            QAction **revealAction = nullptr,
                            QAction **copyAction = nullptr) const;

signals:
    void launchRequested(const ExecutableRecord &record, bool asAdmin);
    void revealRequested(const ExecutableRecord &record);
    void settingsRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void buildUi();
    void scheduleSearch();
    void performSearch();
    void moveSelection(int delta);
    void activateCurrent(bool reveal, bool asAdmin);
    void updateGeometryForResults(int rowCount, bool emptyQuery);
    void showContextMenu(const QPoint &position);

    SearchEngine *m_engine = nullptr;
    int m_maxResults = 12;
    bool m_hideOnFocusLost = true;

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
