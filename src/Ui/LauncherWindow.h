#pragma once

#include "Core/PluginHost.h"
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
class PluginHost;
class ResultModel;
class SearchService;

/** 面板当前在做什么：找程序，还是把输入交给某个插件。 */
enum class LauncherMode
{
    Programs,
    Plugin
};

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

    /** 插件宿主：设置后面板才能进入插件模式。 */
    void setPluginHost(PluginHost *host);

    /** 进入插件模式并弹出面板（由插件热键触发）。 */
    void beginPluginSession(const QString &pluginId, const QString &displayName,
                            const QString &placeholder);
    /** 结束插件模式（Esc / 失焦 / 动作执行完）。 */
    void endPluginSession();
    LauncherMode mode() const { return m_mode; }
    QString pluginId() const { return m_pluginId; }

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
    /** 插件行被回车/双击执行：action 见 sdk/listery_plugin_api.h。 */
    void pluginActionRequested(const QString &action);
    /** 插件模式已结束，宿主可以把状态栏恢复成索引状态。 */
    void pluginSessionClosed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void applyResults(quint64 sequence, const QString &query,
                      const QVector<SearchResult> &results, qint64 elapsedMs);
    void applyPluginResults(quint64 sequence, QString pluginId, QString query,
                            QVector<PluginRow> rows, QString errorMessage);

private:
    void buildUi();
    void scheduleSearch();
    void performSearch();
    void moveSelection(int delta);
    void activateCurrent(bool reveal, bool asAdmin);
    void updateGeometryForResults(int rowCount, bool emptyQuery);
    void showContextMenu(const QPoint &position);
    void enterPluginMode(const QString &pluginId, const QString &displayName,
                         const QString &placeholder);

    SearchService *m_service = nullptr;
    PluginHost *m_pluginHost = nullptr;
    LauncherMode m_mode = LauncherMode::Programs;
    QString m_pluginId;
    QString m_pluginName;
    QString m_statusBeforePlugin;
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
