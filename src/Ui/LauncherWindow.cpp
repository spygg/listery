#include "Ui/LauncherWindow.h"

#include "Core/LaunchService.h"
#include "Platform/WindowUtils.h"
#include "Ui/ResultDelegate.h"
#include "Ui/ResultModel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
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
} // namespace

LauncherWindow::LauncherWindow(SearchEngine *engine, int maxResults, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
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
    if (!m_engine)
        return;

    const QString query = m_edit->text().trimmed();
    const int previousRow = m_list->currentIndex().row();

    // 没有输入时不展示任何候选：面板只保留输入框
    if (query.isEmpty()) {
        m_model->clearResults();
        m_list->setCurrentIndex(QModelIndex());
        updateGeometryForResults(0, true);
        return;
    }

    const QVector<SearchResult> results = m_engine->search(query, m_maxResults);
    m_model->setResults(results);

    if (!results.isEmpty()) {
        const int target = qBound(0, previousRow < 0 ? 0 : previousRow, results.size() - 1);
        m_list->setCurrentIndex(m_model->index(target));
        m_list->scrollTo(m_model->index(target), QAbstractItemView::EnsureVisible);
    } else {
        m_list->setCurrentIndex(QModelIndex());
    }

    updateGeometryForResults(results.size(), false);
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
    const SearchResult &result = m_model->resultAt(index.row());
    if (result.record.path.isEmpty())
        return;

    QAction *adminAction = nullptr;
    QAction *revealAction = nullptr;
    QAction *copyAction = nullptr;
    QMenu *menu = createResultMenu(result, &adminAction, &revealAction, &copyAction);
    QAction *chosen = menu->exec(m_list->viewport()->mapToGlobal(position));
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

    int contentHeight = kEditHeight;
    if (noRows) {
        m_list->setFixedHeight(0);
    } else {
        const int visibleRows = qMin(rowCount, m_maxResults);
        m_list->setFixedHeight(visibleRows * kRowHeight);
        contentHeight += 1 + m_list->height();
    }
    if (!emptyQuery)
        contentHeight += kFooterHeight;

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
