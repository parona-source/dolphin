// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/BreakpointWidget.h"

#include <QApplication>
#include <QHeaderView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DolphinQt/Debugger/BreakpointDialog.h"
#include "DolphinQt/Debugger/MemoryWidget.h"
#include "DolphinQt/QtUtils/SetWindowDecorations.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

// Qt constants
namespace
{
enum CustomRole
{
  ADDRESS_ROLE = Qt::UserRole,
  IS_MEMCHECK_ROLE
};

enum TableColumns
{
  ENABLED_COLUMN = 0,
  TYPE_COLUMN = 1,
  SYMBOL_COLUMN = 2,
  ADDRESS_COLUMN = 3,
  END_ADDRESS_COLUMN = 4,
  BREAK_COLUMN = 5,
  LOG_COLUMN = 6,
  READ_COLUMN = 7,
  WRITE_COLUMN = 8,
  CONDITION_COLUMN = 9,
};
}  // namespace

// Fix icons not centering properly in a QTableWidget.
class CustomDelegate : public QStyledItemDelegate
{
public:
  CustomDelegate(BreakpointWidget* parent) : QStyledItemDelegate(parent) {}

private:
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
  {
    Q_ASSERT(index.isValid());

    // Fetch normal drawing logic.
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    // Disable drawing icon the normal way.
    opt.icon = QIcon();
    opt.decorationSize = QSize(0, 0);

    // Default draw command for paint.
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, 0);

    // Draw pixmap at the center of the tablewidget cell
    QPixmap pix = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
    if (!pix.isNull())
    {
      const QRect r = option.rect;
      const QPoint p = QPoint((r.width() - pix.width()) / 2, (r.height() - pix.height()) / 2);
      painter->drawPixmap(r.topLeft() + p, pix);
    }
  }
};

BreakpointWidget::BreakpointWidget(QWidget* parent)
    : QDockWidget(parent), m_system(Core::System::GetInstance())
{
  setWindowTitle(tr("Breakpoints"));
  setObjectName(QStringLiteral("breakpoints"));

  setHidden(!Settings::Instance().IsBreakpointsVisible() ||
            !Settings::Instance().IsDebugModeEnabled());

  setAllowedAreas(Qt::AllDockWidgetAreas);

  CreateWidgets();

  auto& settings = Settings::GetQSettings();

  restoreGeometry(settings.value(QStringLiteral("breakpointwidget/geometry")).toByteArray());
  // macOS: setHidden() needs to be evaluated before setFloating() for proper window presentation
  // according to Settings
  setFloating(settings.value(QStringLiteral("breakpointwidget/floating")).toBool());

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    UpdateButtonsEnabled();
    if (state == Core::State::Uninitialized)
      Update();
  });

  connect(&Settings::Instance(), &Settings::BreakpointsVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });

  connect(&Settings::Instance(), &Settings::DebugModeToggled, this, [this](bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsBreakpointsVisible());
  });

  connect(&Settings::Instance(), &Settings::ThemeChanged, this, &BreakpointWidget::UpdateIcons);
  UpdateIcons();
}

BreakpointWidget::~BreakpointWidget()
{
  auto& settings = Settings::GetQSettings();

  settings.setValue(QStringLiteral("breakpointwidget/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("breakpointwidget/floating"), isFloating());
}

void BreakpointWidget::CreateWidgets()
{
  m_toolbar = new QToolBar;
  m_toolbar->setContentsMargins(0, 0, 0, 0);
  m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  m_table = new QTableWidget;
  m_table->setItemDelegate(new CustomDelegate(this));
  m_table->setTabKeyNavigation(false);
  m_table->setContentsMargins(0, 0, 0, 0);
  m_table->setColumnCount(10);
  m_table->setSelectionMode(QAbstractItemView::NoSelection);
  m_table->verticalHeader()->hide();

  connect(m_table, &QTableWidget::itemClicked, this, &BreakpointWidget::OnClicked);
  connect(m_table, &QTableWidget::customContextMenuRequested, this,
          &BreakpointWidget::OnContextMenu);

  m_table->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);

  auto* layout = new QVBoxLayout;

  layout->addWidget(m_toolbar);
  layout->addWidget(m_table);
  layout->setContentsMargins(2, 2, 2, 2);
  layout->setSpacing(0);

  m_new = m_toolbar->addAction(tr("New"), this, &BreakpointWidget::OnNewBreakpoint);
  m_clear = m_toolbar->addAction(tr("Clear"), this, &BreakpointWidget::OnClear);

  m_load = m_toolbar->addAction(tr("Load"), this, &BreakpointWidget::OnLoad);
  m_save = m_toolbar->addAction(tr("Save"), this, &BreakpointWidget::OnSave);

  m_new->setEnabled(false);
  m_load->setEnabled(false);
  m_save->setEnabled(false);

  QWidget* widget = new QWidget;
  widget->setLayout(layout);

  setWidget(widget);
}

void BreakpointWidget::UpdateIcons()
{
  m_new->setIcon(Resources::GetThemeIcon("debugger_add_breakpoint"));
  m_clear->setIcon(Resources::GetThemeIcon("debugger_clear"));
  m_load->setIcon(Resources::GetThemeIcon("debugger_load"));
  m_save->setIcon(Resources::GetThemeIcon("debugger_save"));
}

void BreakpointWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetBreakpointsVisible(false);
}

void BreakpointWidget::showEvent(QShowEvent* event)
{
  UpdateButtonsEnabled();
  Update();
}

void BreakpointWidget::OnClicked(QTableWidgetItem* item)
{
  if (!item)
    return;

  if (item->column() == ADDRESS_COLUMN || item->column() == END_ADDRESS_COLUMN)
    return;

  const u32 address = static_cast<u32>(m_table->item(item->row(), 0)->data(ADDRESS_ROLE).toUInt());

  if (item->column() == ENABLED_COLUMN)
  {
    if (item->data(IS_MEMCHECK_ROLE).toBool())
      m_system.GetPowerPC().GetMemChecks().ToggleBreakPoint(address);
    else
      m_system.GetPowerPC().GetBreakPoints().ToggleBreakPoint(address);

    emit BreakpointsChanged();
    Update();
    return;
  }

  if (m_table->item(item->row(), 0)->data(IS_MEMCHECK_ROLE).toBool())
    EditMBP(address, item->column());
  else
    EditBreakpoint(address, item->column());
}

void BreakpointWidget::UpdateButtonsEnabled()
{
  if (!isVisible())
    return;

  const bool is_initialised = Core::GetState(m_system) != Core::State::Uninitialized;
  m_new->setEnabled(is_initialised);
  m_load->setEnabled(is_initialised);
  m_save->setEnabled(is_initialised);
}

void BreakpointWidget::Update()
{
  if (!isVisible())
    return;

  m_table->clear();
  m_table->setHorizontalHeaderLabels({tr("Active"), tr("Type"), tr("Function"), tr("Address"),
                                      tr("End Addr"), tr("Break"), tr("Log"), tr("Read"),
                                      tr("Write"), tr("Condition")});
  m_table->horizontalHeader()->setStretchLastSection(true);

  // Get row height for icons
  m_table->setRowCount(1);
  const int height = m_table->rowHeight(0);
  m_table->setRowCount(0);

  // Create icon based on row height, downscaled for whitespace padding.
  const int downscale = static_cast<int>(0.8 * height);
  QPixmap enabled_icon =
      Resources::GetThemeIcon("debugger_breakpoint").pixmap(QSize(downscale, downscale));

  const auto create_item = [](const QString& string = {}) {
    QTableWidgetItem* item = new QTableWidgetItem(string);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
  };

  QTableWidgetItem empty_item = QTableWidgetItem();
  empty_item.setFlags(Qt::NoItemFlags);
  QTableWidgetItem icon_item = QTableWidgetItem();
  icon_item.setData(Qt::DecorationRole, enabled_icon);

  auto& power_pc = m_system.GetPowerPC();
  auto& breakpoints = power_pc.GetBreakPoints();
  auto& memchecks = power_pc.GetMemChecks();
  auto& ppc_symbol_db = power_pc.GetSymbolDB();

  int i = 0;

  // Breakpoints
  for (const auto& bp : breakpoints.GetBreakPoints())
  {
    m_table->setRowCount(i + 1);

    auto* active = create_item();

    active->setData(ADDRESS_ROLE, bp.address);
    active->setData(IS_MEMCHECK_ROLE, false);
    if (bp.is_enabled)
      active->setData(Qt::DecorationRole, enabled_icon);

    m_table->setItem(i, ENABLED_COLUMN, active);
    m_table->setItem(i, TYPE_COLUMN, create_item(QStringLiteral("BP")));

    if (const Common::Symbol* const symbol = ppc_symbol_db.GetSymbolFromAddr(bp.address))
      m_table->setItem(i, SYMBOL_COLUMN, create_item(QString::fromStdString(symbol->name)));

    m_table->setItem(i, ADDRESS_COLUMN,
                     create_item(QStringLiteral("%1").arg(bp.address, 8, 16, QLatin1Char('0'))));

    m_table->setItem(i, BREAK_COLUMN, bp.break_on_hit ? icon_item.clone() : empty_item.clone());
    m_table->setItem(i, LOG_COLUMN, bp.log_on_hit ? icon_item.clone() : empty_item.clone());
    m_table->setItem(i, READ_COLUMN, empty_item.clone());
    m_table->setItem(i, WRITE_COLUMN, empty_item.clone());

    QString condition;

    if (bp.condition)
      condition = QString::fromStdString(bp.condition->GetText());

    m_table->setItem(i, CONDITION_COLUMN, create_item(condition));

    i++;
  }

  // Memory Breakpoints
  for (const auto& mbp : memchecks.GetMemChecks())
  {
    m_table->setRowCount(i + 1);
    auto* active = create_item();
    active->setData(ADDRESS_ROLE, mbp.start_address);
    active->setData(IS_MEMCHECK_ROLE, true);
    if (mbp.is_enabled)
      active->setData(Qt::DecorationRole, enabled_icon);

    m_table->setItem(i, ENABLED_COLUMN, active);
    m_table->setItem(i, TYPE_COLUMN, create_item(QStringLiteral("MBP")));

    if (const Common::Symbol* const symbol = ppc_symbol_db.GetSymbolFromAddr(mbp.start_address))
      m_table->setItem(i, SYMBOL_COLUMN, create_item(QString::fromStdString(symbol->name)));

    if (mbp.is_ranged)
    {
      m_table->setItem(i, ADDRESS_COLUMN,
                       create_item(QStringLiteral("%1 - %2")
                                       .arg(mbp.start_address, 8, 16, QLatin1Char('0'))
                                       .arg(mbp.end_address, 8, 16, QLatin1Char('0'))));
    }
    else
    {
      m_table->setItem(
          i, ADDRESS_COLUMN,
          create_item(QStringLiteral("%1").arg(mbp.start_address, 8, 16, QLatin1Char('0'))));
    }

    m_table->setItem(i, BREAK_COLUMN, mbp.break_on_hit ? icon_item.clone() : empty_item.clone());
    m_table->setItem(i, LOG_COLUMN, mbp.log_on_hit ? icon_item.clone() : empty_item.clone());
    m_table->setItem(i, READ_COLUMN, mbp.is_break_on_read ? icon_item.clone() : empty_item.clone());
    m_table->setItem(i, WRITE_COLUMN,
                     mbp.is_break_on_write ? icon_item.clone() : empty_item.clone());

    QString condition;

    if (mbp.condition)
      condition = QString::fromStdString(mbp.condition->GetText());

    m_table->setItem(i, CONDITION_COLUMN, create_item(condition));

    i++;
  }

  m_table->resizeColumnToContents(ENABLED_COLUMN);
  m_table->resizeColumnToContents(BREAK_COLUMN);
  m_table->resizeColumnToContents(LOG_COLUMN);
  m_table->resizeColumnToContents(READ_COLUMN);
  m_table->resizeColumnToContents(WRITE_COLUMN);
}

void BreakpointWidget::OnClear()
{
  m_system.GetPowerPC().GetDebugInterface().ClearAllBreakpoints();
  {
    const QSignalBlocker blocker(Settings::Instance());
    m_system.GetPowerPC().GetDebugInterface().ClearAllMemChecks();
  }

  m_table->setRowCount(0);

  emit BreakpointsChanged();
  Update();
}

void BreakpointWidget::OnNewBreakpoint()
{
  BreakpointDialog* dialog = new BreakpointDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose, true);
  SetQWidgetWindowDecorations(dialog);
  dialog->exec();
}

void BreakpointWidget::OnEditBreakpoint(u32 address, bool is_instruction_bp)
{
  if (is_instruction_bp)
  {
    auto* dialog =
        new BreakpointDialog(this, m_system.GetPowerPC().GetBreakPoints().GetBreakpoint(address));
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    SetQWidgetWindowDecorations(dialog);
    dialog->exec();
  }
  else
  {
    auto* dialog =
        new BreakpointDialog(this, m_system.GetPowerPC().GetMemChecks().GetMemCheck(address));
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    SetQWidgetWindowDecorations(dialog);
    dialog->exec();
  }

  emit BreakpointsChanged();
  Update();
}

void BreakpointWidget::OnLoad()
{
  Common::IniFile ini;
  if (!ini.Load(File::GetUserPath(D_GAMESETTINGS_IDX) + SConfig::GetInstance().GetGameID() + ".ini",
                false))
  {
    return;
  }

  BreakPoints::TBreakPointsStr new_bps;
  if (ini.GetLines("BreakPoints", &new_bps, false))
  {
    auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
    breakpoints.Clear();
    breakpoints.AddFromStrings(new_bps);
  }

  MemChecks::TMemChecksStr new_mcs;
  if (ini.GetLines("MemoryBreakPoints", &new_mcs, false))
  {
    auto& memchecks = m_system.GetPowerPC().GetMemChecks();
    memchecks.Clear();
    const QSignalBlocker blocker(Settings::Instance());
    memchecks.AddFromStrings(new_mcs);
  }

  emit BreakpointsChanged();
  Update();
}

void BreakpointWidget::OnSave()
{
  Common::IniFile ini;
  ini.Load(File::GetUserPath(D_GAMESETTINGS_IDX) + SConfig::GetInstance().GetGameID() + ".ini",
           false);
  ini.SetLines("BreakPoints", m_system.GetPowerPC().GetBreakPoints().GetStrings());
  ini.SetLines("MemoryBreakPoints", m_system.GetPowerPC().GetMemChecks().GetStrings());
  ini.Save(File::GetUserPath(D_GAMESETTINGS_IDX) + SConfig::GetInstance().GetGameID() + ".ini");
}

void BreakpointWidget::OnContextMenu(const QPoint& pos)
{
  const auto row = m_table->rowAt(pos.y());
  const auto& selected_item = m_table->item(row, 0);
  if (selected_item == nullptr)
    return;

  const auto bp_address = static_cast<u32>(selected_item->data(ADDRESS_ROLE).toUInt());
  const auto is_memory_breakpoint = selected_item->data(IS_MEMCHECK_ROLE).toBool();

  auto* menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  if (!is_memory_breakpoint)
  {
    const auto& inst_breakpoints = m_system.GetPowerPC().GetBreakPoints().GetBreakPoints();
    const auto bp_iter =
        std::find_if(inst_breakpoints.begin(), inst_breakpoints.end(),
                     [bp_address](const auto& bp) { return bp.address == bp_address; });
    if (bp_iter == inst_breakpoints.end())
      return;

    menu->addAction(tr("Show in Code"), [this, bp_address] { emit ShowCode(bp_address); });
    menu->addAction(tr("Edit..."), [this, bp_address] { OnEditBreakpoint(bp_address, true); });
    menu->addAction(tr("Delete"), [this, &bp_address]() {
      m_system.GetPowerPC().GetBreakPoints().Remove(bp_address);
      emit BreakpointsChanged();
      Update();
    });
  }
  else
  {
    const auto& memory_breakpoints = m_system.GetPowerPC().GetMemChecks().GetMemChecks();
    const auto mb_iter =
        std::find_if(memory_breakpoints.begin(), memory_breakpoints.end(),
                     [bp_address](const auto& bp) { return bp.start_address == bp_address; });
    if (mb_iter == memory_breakpoints.end())
      return;

    menu->addAction(tr("Show in Memory"), [this, bp_address] { emit ShowMemory(bp_address); });
    menu->addAction(tr("Edit..."), [this, bp_address] { OnEditBreakpoint(bp_address, false); });
    menu->addAction(tr("Delete"), [this, &bp_address]() {
      const QSignalBlocker blocker(Settings::Instance());
      m_system.GetPowerPC().GetMemChecks().Remove(bp_address);
      emit BreakpointsChanged();
      Update();
    });
  }

  menu->exec(QCursor::pos());
}

void BreakpointWidget::AddBP(u32 addr)
{
  AddBP(addr, false, true, true, {});
}

void BreakpointWidget::AddBP(u32 addr, bool temp, bool break_on_hit, bool log_on_hit,
                             const QString& condition)
{
  m_system.GetPowerPC().GetBreakPoints().Add(
      addr, temp, break_on_hit, log_on_hit,
      !condition.isEmpty() ? Expression::TryParse(condition.toUtf8().constData()) : std::nullopt);

  emit BreakpointsChanged();
  Update();
}

void BreakpointWidget::EditBreakpoint(u32 address, int edit, std::optional<QString> string)
{
  TBreakPoint bp;
  const TBreakPoint* old_bp = m_system.GetPowerPC().GetBreakPoints().GetBreakpoint(address);
  bp.is_enabled = edit == ENABLED_COLUMN ? !old_bp->is_enabled : old_bp->is_enabled;
  bp.log_on_hit = edit == LOG_COLUMN ? !old_bp->log_on_hit : old_bp->log_on_hit;
  bp.break_on_hit = edit == BREAK_COLUMN ? !old_bp->break_on_hit : old_bp->break_on_hit;

  if (edit == ADDRESS_COLUMN && string.has_value())
  {
    bool ok;
    const u32 new_address = string.value().toUInt(&ok, 16);
    if (!ok)
      return;

    bp.address = new_address;
  }
  else
  {
    bp.address = address;
  }

  if (edit == CONDITION_COLUMN && string.has_value())
    bp.condition = Expression::TryParse(string.value().toUtf8().constData());
  else if (old_bp->condition.has_value() && edit != CONDITION_COLUMN)
    bp.condition = Expression::TryParse(old_bp->condition.value().GetText());

  // Unlike MBPs it Add() for TBreakpoint doesn't check to see if it already exists.
  m_system.GetPowerPC().GetBreakPoints().Remove(address);
  m_system.GetPowerPC().GetBreakPoints().Add(std::move(bp));

  emit BreakpointsChanged();
  Update();
}

void BreakpointWidget::AddAddressMBP(u32 addr, bool on_read, bool on_write, bool do_log,
                                     bool do_break, const QString& condition)
{
  TMemCheck check;

  check.start_address = addr;
  check.end_address = addr;
  check.is_ranged = false;
  check.is_break_on_read = on_read;
  check.is_break_on_write = on_write;
  check.log_on_hit = do_log;
  check.break_on_hit = do_break;
  check.condition =
      !condition.isEmpty() ? Expression::TryParse(condition.toUtf8().constData()) : std::nullopt;
  {
    const QSignalBlocker blocker(Settings::Instance());
    m_system.GetPowerPC().GetMemChecks().Add(std::move(check));
  }

  emit BreakpointsChanged();
  Update();
}

void BreakpointWidget::AddRangedMBP(u32 from, u32 to, bool on_read, bool on_write, bool do_log,
                                    bool do_break, const QString& condition)
{
  TMemCheck check;

  check.start_address = from;
  check.end_address = to;
  check.is_ranged = true;
  check.is_break_on_read = on_read;
  check.is_break_on_write = on_write;
  check.log_on_hit = do_log;
  check.break_on_hit = do_break;
  check.condition =
      !condition.isEmpty() ? Expression::TryParse(condition.toUtf8().constData()) : std::nullopt;
  {
    const QSignalBlocker blocker(Settings::Instance());
    m_system.GetPowerPC().GetMemChecks().Add(std::move(check));
  }

  emit BreakpointsChanged();
  Update();
}

void BreakpointWidget::EditMBP(u32 address, int edit, std::optional<QString> string)
{
  bool address_changed = false;

  TMemCheck mbp;
  const TMemCheck* old_mbp = m_system.GetPowerPC().GetMemChecks().GetMemCheck(address);
  mbp.is_enabled = edit == ENABLED_COLUMN ? !old_mbp->is_enabled : old_mbp->is_enabled;
  mbp.log_on_hit = edit == LOG_COLUMN ? !old_mbp->log_on_hit : old_mbp->log_on_hit;
  mbp.break_on_hit = edit == BREAK_COLUMN ? !old_mbp->break_on_hit : old_mbp->break_on_hit;
  mbp.is_break_on_read =
      edit == READ_COLUMN ? !old_mbp->is_break_on_read : old_mbp->is_break_on_read;
  mbp.is_break_on_write =
      edit == WRITE_COLUMN ? !old_mbp->is_break_on_write : old_mbp->is_break_on_write;

  if ((edit == ADDRESS_COLUMN || edit == END_ADDRESS_COLUMN) && string.has_value())
  {
    bool ok;
    const u32 new_address = string.value().toUInt(&ok, 16);
    if (!ok)
      return;

    if (edit == ADDRESS_COLUMN)
    {
      mbp.start_address = new_address;
      mbp.end_address = old_mbp->end_address;
      address_changed = true;
    }
    else if (edit == END_ADDRESS_COLUMN)
    {
      // Will update existing mbp, so does not use address_changed bool.
      mbp.start_address = old_mbp->start_address;
      mbp.end_address = new_address;
    }
  }
  else
  {
    mbp.start_address = old_mbp->start_address;
    mbp.end_address = old_mbp->end_address;
  }

  mbp.is_ranged = mbp.start_address != mbp.end_address;

  if (edit == CONDITION_COLUMN && string.has_value())
    mbp.condition = Expression::TryParse(string.value().toUtf8().constData());
  else if (old_mbp->condition.has_value() && edit != CONDITION_COLUMN)
    mbp.condition = Expression::TryParse(old_mbp->condition.value().GetText());

  {
    const QSignalBlocker blocker(Settings::Instance());
    m_system.GetPowerPC().GetMemChecks().Add(std::move(mbp));
    if (address_changed)
      m_system.GetPowerPC().GetMemChecks().Remove(address);
  }

  emit BreakpointsChanged();
  Update();
}
