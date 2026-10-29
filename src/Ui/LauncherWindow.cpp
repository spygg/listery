#include "Ui/LauncherWindow.h"

#include "Core/LaunchService.h"
#include "Core/SearchService.h"
#include "Platform/WindowUtils.h"
#include "Ui/ResultDelegate.h"
#include "Ui/ResultModel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QEventLoop>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {
const int kPanelWidth = 760;
const int kEditHeight = 62;
const int kRowHeight = 54;
const int kShadowMarginX = 26;
const int kShadowMarginTop = 22;
const int kShadowMarginBottom = 26;
const int kFooterHeight = 36;
/** 卡片自身的上下边框（QFrame 1px × 2）：布局的最小高度会把它算进去，手算时也要计入。 */
const int kCardBorder = 2;
} // namespace

LauncherWindow::LauncherWindow(SearchService *service, int maxResults, QWidget *parent)
    : QWidget(parent)
    , m_service(service)
    , m_maxResults(maxResults)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                   | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setWindowTitle(QStringLiteral("Listery"));
    setFixedWidth(kPanelWidth);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(35);
    connect(m_debounce, &QTimer::timeout, this, &LauncherWindow::performSearch);

    if (m_service)
        connect(m_service, &SearchService::resultsReady, this, &LauncherWindow::applyResults);

    buildUi();
    qApp->installEventFilter(this);
}

LauncherWindow::~LauncherWindow()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

void LauncherWindow::buildUi()
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(kShadowMarginX, kShadowMarginTop,
                                    kShadowMarginX, kShadowMarginBottom);
    outerLayout->setSpacing(0);
    // 默认的 SetDefaultConstraint 会把窗口 minimumHeight 抬到布局当前的最小高度，
    // 而这个值只会随展开状态上抬、不会随列表隐藏回落，之后 resize() 会被它钳住，
    // 表现就是“Enter 启动后再唤起面板缩不回输入框高度”。改由我们自己算高度。
    outerLayout->setSizeConstraint(QLayout::SetNoConstraint);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("launcherCard"));
    m_card->setFrameShape(QFrame::NoFrame);

    auto *shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(38);
    shadow->setOffset(0, 10);
    shadow->setColor(QColor(15, 23, 42, 90));
    m_card->setGraphicsEffect(shadow);

    outerLayout->addWidget(m_card);

    auto *cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    m_edit = new QLineEdit(m_card);
    m_edit->setObjectName(QStringLiteral("searchEdit"));
    m_edit->setPlaceholderText(QStringLiteral("输入程序名，例如 chrome、excel、git"));
    m_edit->setClearButtonEnabled(false);
    m_edit->setFixedHeight(kEditHeight);
    m_edit->installEventFilter(this);
    connect(m_edit, &QLineEdit::textChanged, this, &LauncherWindow::scheduleSearch);
    cardLayout->addWidget(m_edit);

    m_separator = new QFrame(m_card);
    m_separator->setObjectName(QStringLiteral("separator"));
    m_separator->setFixedHeight(1);
    cardLayout->addWidget(m_separator);

    m_list = new QListView(m_card);
    m_list->setObjectName(QStringLiteral("resultList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(true);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setModel(m_model = new ResultModel(this));
    m_list->setItemDelegate(new ResultDelegate(this));
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListView::doubleClicked, this, [this]() {
        activateCurrent(false, false);
    });
    connect(m_list, &QWidget::customContextMenuRequested,
            this, &LauncherWindow::showContextMenu);
    cardLayout->addWidget(m_list);

    m_footer = new QWidget(m_card);
    m_footer->setObjectName(QStringLiteral("footer"));
    m_footer->setFixedHeight(kFooterHeight);
    auto *footerLayout = new QHBoxLayout(m_footer);
    footerLayout->setContentsMargins(20, 0, 18, 0);
    footerLayout->setSpacing(12);

    m_status = new QLabel(m_footer);
    m_status->setObjectName(QStringLiteral("statusLabel"));
    m_status->setText(QStringLiteral("索引尚未建立"));
    m_status->setMaximumWidth(420);
    footerLayout->addWidget(m_status);
    footerLayout->addStretch(1);

    m_hints = new QLabel(m_footer);
    m_hints->setObjectName(QStringLiteral("hintLabel"));
    m_hints->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_hints->setText(QStringLiteral("↑↓ 选择 · Enter 运行 · Ctrl+Enter 定位 · Shift+Enter 管理员 · Esc 隐藏"));
    footerLayout->addWidget(m_hints);
    cardLayout->addWidget(m_footer);

    setStyleSheet(QStringLiteral(R"(
        #launcherCard {
            background: #FFFFFF;
            border: 1px solid #E4E8EF;
            border-radius: 14px;
        }
        #searchEdit {
            background: transparent;
            border: none;
            padding: 0 22px;
            font-size: 21px;
            color: #1F2329;
            selection-background-color: #CFE1FF;
            selection-color: #1F2329;
        }
        #separator {
            background: #EDEFF3;
        }
        #resultList {
            background: transparent;
            border: none;
            outline: none;
        }
        #footer {
            background: transparent;
        }
        #statusLabel {
            color: #8A919B;
            font-size: 11px;
        }
        #hintLabel {
            color: #A2A8B0;
            font-size: 11px;
        }
        QScrollBar:vertical {
            width: 8px;
            background: transparent;
            margin: 4px 2px 4px 0;
        }
        QScrollBar::handle:vertical {
            background: #D6DBE3;
            border-radius: 4px;
            min-height: 28px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: transparent;
        }
    )"));
}

void LauncherWindow::popup()
{
    {
        QSignalBlocker blocker(m_edit);
        m_edit->clear();
    }
    performSearch();
    WindowUtils::placeCenterTop(this);
    show();
    raise();
    WindowUtils::forceActivate(this);
    m_edit->setFocus(Qt::ShortcutFocusReason);
}

void LauncherWindow::dismiss()
{
    if (!isVisible())
        return;
    hide();
    m_model->clearResults();
    {
        QSignalBlocker blocker(m_edit);
        m_edit->clear();
    }
    // 隐藏前把面板收回输入框高度：否则下次唤起会带着上一次展开的尺寸出现
    updateGeometryForResults(0, true);
}

void LauncherWindow::refreshResults()
{
    if (isVisible())
        performSearch();
}

void LauncherWindow::setMaxResults(int maxResults)
{
    m_maxResults = qBound(5, maxResults, 30);
    if (isVisible())
        performSearch();
}

void LauncherWindow::setHideOnFocusLost(bool on)
{
    m_hideOnFocusLost = on;
}

void LauncherWindow::setStatusText(const QString &text)
{
    m_status->setText(text);
    m_status->setToolTip(text);
}

void LauncherWindow::setQueryForPreview(const QString &query)
{
    {
        QSignalBlocker blocker(m_edit);
        m_edit->setText(query);
    }
    performSearch();
}

void LauncherWindow::scheduleSearch()
{
    m_debounce->start();
}

void LauncherWindow::performSearch()
{
    const QString query = m_edit->text().trimmed();

    // 没有输入时不展示任何候选：面板只保留输入框
    if (query.isEmpty()) {
        m_pendingSequence = 0;
        m_appliedSequence = 0;
        m_model->clearResults();
        m_list->setCurrentIndex(QModelIndex());
        updateGeometryForResults(0, true);
        return;
    }

    if (!m_service)
        return;
    m_pendingSequence = m_service->search(query, m_maxResults);
}

void LauncherWindow::applyResults(quint64 sequence, const QString &query,
                                  const QVector<SearchResult> &results, qint64 elapsedMs)
{
    Q_UNUSED(elapsedMs)
    // 输入已经变了：只认最新一次请求的结果，过期结果直接丢弃
    if (sequence != m_pendingSequence || query != m_edit->text().trimmed())
        return;

    const int previousRow = m_list->currentIndex().row();
    m_model->setResults(results);

    if (!results.isEmpty()) {
        const int target = qBound(0, previousRow < 0 ? 0 : previousRow, results.size() - 1);
        m_list->setCurrentIndex(m_model->index(target));
        m_list->scrollTo(m_model->index(target), QAbstractItemView::EnsureVisible);
    } else {
        m_list->setCurrentIndex(QModelIndex());
    }

    updateGeometryForResults(results.size(), false);
    m_appliedSequence = sequence;
    emit searchApplied();
}

bool LauncherWindow::waitForSearch(int timeoutMs)
{
    if (m_pendingSequence == 0 || m_appliedSequence == m_pendingSequence)
        return true;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &LauncherWindow::searchApplied, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    return m_appliedSequence == m_pendingSequence;
}

QMenu *LauncherWindow::createResultMenu(const SearchResult &result,
                                        QAction **adminAction,
                                        QAction **revealAction,
                                        QAction **copyAction) const
{
    Q_UNUSED(result)
    QMenu *menu = new QMenu(const_cast<LauncherWindow *>(this));
    QAction *admin = menu->addAction(QStringLiteral("以管理员身份运行"));
    QAction *reveal = menu->addAction(QStringLiteral("打开所在位置"));
    QAction *copy = menu->addAction(QStringLiteral("复制路径到剪贴板"));
    if (adminAction)
        *adminAction = admin;
    if (revealAction)
        *revealAction = reveal;
    if (copyAction)
        *copyAction = copy;
    return menu;
}

void LauncherWindow::showContextMenu(const QPoint &position)
{
    const QModelIndex index = m_list->indexAt(position);
    if (!index.isValid() || index.row() < 0 || index.row() >= m_model->count())
        return;

    m_list->setCurrentIndex(index);
    // exec() 会跑嵌套事件循环，期间扫描结束刷新或窗口失焦隐藏都可能清空模型，必须持有副本
    const SearchResult result = m_model->resultAt(index.row());
    if (result.record.path.isEmpty())
        return;

    QAction *adminAction = nullptr;
    QAction *revealAction = nullptr;
    QAction *copyAction = nullptr;
    QMenu *menu = createResultMenu(result, &adminAction, &revealAction, &copyAction);
    m_contextMenuOpen = true;
    QAction *chosen = menu->exec(m_list->viewport()->mapToGlobal(position));
    m_contextMenuOpen = false;
    delete menu;

    if (chosen == adminAction) {
        emit launchRequested(result.record, true);
    } else if (chosen == revealAction) {
        emit revealRequested(result.record);
    } else if (chosen == copyAction) {
        QGuiApplication::clipboard()->setText(result.record.path);
        dismiss();
    }
}

void LauncherWindow::moveSelection(int delta)
{
    const int count = m_model->count();
    if (count <= 0)
        return;

    int row = m_list->currentIndex().row();
    if (row < 0)
        row = delta > 0 ? -1 : count;
    const int target = qBound(0, row + delta, count - 1);
    const QModelIndex index = m_model->index(target);
    m_list->setCurrentIndex(index);
    m_list->scrollTo(index, QAbstractItemView::EnsureVisible);
}

void LauncherWindow::activateCurrent(bool reveal, bool asAdmin)
{
    int row = m_list->currentIndex().row();
    if (row < 0)
        row = 0;
    if (row < 0 || row >= m_model->count())
        return;

    const SearchResult &result = m_model->resultAt(row);
    if (result.record.path.isEmpty())
        return;

    if (reveal)
        emit revealRequested(result.record);
    else
        emit launchRequested(result.record, asAdmin);
}

void LauncherWindow::updateGeometryForResults(int rowCount, bool emptyQuery)
{
    const bool noRows = rowCount <= 0;

    // 空输入：只有输入框；有输入但无候选：输入框 + 状态栏；有候选：展开列表
    m_list->setVisible(!noRows);
    m_separator->setVisible(!noRows);
    m_footer->setVisible(!emptyQuery);

    int contentHeight = kEditHeight + kCardBorder;
    if (noRows) {
        m_list->setFixedHeight(0);
    } else {
        const int visibleRows = qMin(rowCount, m_maxResults);
        m_list->setFixedHeight(visibleRows * kRowHeight);
        contentHeight += 1 + m_list->height();
    }
    if (!emptyQuery)
        contentHeight += kFooterHeight;

    // 先清掉最小高度再 resize：布局一旦把 minimumHeight 抬起来，resize() 会被钳住
    setMinimumHeight(0);
    resize(kPanelWidth, contentHeight + kShadowMarginTop + kShadowMarginBottom);
}

bool LauncherWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Escape:
            dismiss();
            return true;
        case Qt::Key_Down:
            moveSelection(1);
            return true;
        case Qt::Key_Up:
            moveSelection(-1);
            return true;
        case Qt::Key_PageDown:
            moveSelection(6);
            return true;
        case Qt::Key_PageUp:
            moveSelection(-6);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activateCurrent(keyEvent->modifiers().testFlag(Qt::ControlModifier),
                            keyEvent->modifiers().testFlag(Qt::ShiftModifier));
            return true;
        default:
            break;
        }
    }

    if (event->type() == QEvent::WindowDeactivate && m_hideOnFocusLost) {
        if (m_contextMenuOpen)
            return QWidget::eventFilter(watched, event);
        QWidget *widget = qobject_cast<QWidget *>(watched);
        if (widget == this) {
            QTimer::singleShot(90, this, [this]() {
                if (!isVisible() || isActiveWindow())
                    return;
                if (rect().contains(mapFromGlobal(QCursor::pos())))
                    return;
                dismiss();
            });
        }
    }

    return QWidget::eventFilter(watched, event);
}

void LauncherWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        return;
    }
    QWidget::keyPressEvent(event);
}
