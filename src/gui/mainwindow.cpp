#include "mainwindow.h"
#include "settingsdialog.h"
#include <QHeaderView>
#include <QDialog>
#include <QScrollArea>

// Display cleanup applied to switch/option labels in the table. Used both when
// populating the table and when reading edits back, so values always round-trip.
static QString cleanLabel(const QString& raw) {
    QString s = raw.trimmed();
    if (s.endsWith(':')) s = s.left(s.length() - 1).trimmed();
    return s.simplified();
}
#include <QDir>
#include <filesystem>
#include <QFileInfoList>
#include <QDateTime>
#include <QTimer>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QClipboard>
#include <QPlainTextEdit>
#include <map>
#include <set>
#include <utility>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setupUI();
    setupMenuBar();
    setWindowTitle(QApplication::applicationName() + " v" +
                   QApplication::applicationVersion());
    resize(1100, 700);

    // Reopen the last working directory on startup, if enabled and still valid.
    // Deferred so the window is shown first and any load log is visible.
    if (appsettings::reopenLastDir()) {
        QString last = appsettings::lastDir();
        if (!last.isEmpty() && QDir(last).exists()) {
            QTimer::singleShot(0, this, [this, last]() {
                loadDirectory(last.toStdString());
            });
        }
    }
}

void MainWindow::setupUI() {
    QWidget* centralWidget = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);

    m_splitter = new QSplitter(Qt::Horizontal);

    // ── Left: game list with checkboxes ──
    m_leftPanel = new QWidget();
    QVBoxLayout* leftLayout = new QVBoxLayout(m_leftPanel);
    leftLayout->setContentsMargins(4,4,4,4);
    leftLayout->addWidget(new QLabel("<center>Titles</center>"));

    m_titleFilter = new QLineEdit();
    m_titleFilter->setPlaceholderText("Filter titles…");
    m_titleFilter->setClearButtonEnabled(true);
    connect(m_titleFilter, &QLineEdit::textChanged, this, &MainWindow::filterTitles);
    leftLayout->addWidget(m_titleFilter);

    // Tristate "select all" checkbox at the top of the title list — toggles the
    // clone-apply checkbox on every title.
    m_selectAllTitles = new QCheckBox("Select all");
    m_selectAllTitles->setTristate(true);
    m_selectAllTitles->setEnabled(false);
    connect(m_selectAllTitles, &QCheckBox::clicked, this, [this]() {
        // Only titles that have a .softdips can be clone-applied to.
        bool on = m_selectAllTitles->checkState() != Qt::Unchecked;
        m_selectAllTitles->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        for (int i = 0; i < m_gameList->count(); i++) {
            bool canSelect = on && i < (int)m_games.size() &&
                             m_games[i].hasSoftDips && m_games[i].softDips;
            m_gameList->item(i)->setCheckState(canSelect ? Qt::Checked : Qt::Unchecked);
        }
    });
    leftLayout->addWidget(m_selectAllTitles);

    m_gameList = new QListWidget();
    m_gameList->setAlternatingRowColors(true);
    m_gameList->setToolTip(
        "Click a title to edit it.\n"
        "Check the box to mark it as a clone target (Tools → Clone Settings).");
    connect(m_gameList, &QListWidget::currentRowChanged, this, &MainWindow::onGameClicked);
    connect(m_gameList, &QListWidget::itemChanged, this,
            &MainWindow::updateTitlesSelectAllState);
    leftLayout->addWidget(m_gameList);

    m_createFromRomBtn = new QPushButton("Create .softdips from P-ROM");
    m_createFromRomBtn->setEnabled(false);
    connect(m_createFromRomBtn, &QPushButton::clicked, this, &MainWindow::createFromRom);
    leftLayout->addWidget(m_createFromRomBtn);

    m_splitter->addWidget(m_leftPanel);

    // ── Right: editor + clone + log ──
    QWidget* rightContainer = new QWidget();
    QVBoxLayout* rightMainLayout = new QVBoxLayout(rightContainer);
    rightMainLayout->setContentsMargins(4,4,4,4);

    m_rightPanel = new QWidget();
    QVBoxLayout* rightLayout = new QVBoxLayout(m_rightPanel);
    rightLayout->setContentsMargins(0,0,0,0);

    m_gameLabel = new QLabel("Select a title from the list");
    m_gameLabel->setStyleSheet("font-weight: bold; font-size: 14px;");
    rightLayout->addWidget(m_gameLabel);

    // Table with QComboBox delegate for Value column
    m_tableView = new QTableView();
    m_model = new QStandardItemModel(0, 2, this);
    m_model->setHorizontalHeaderLabels({"Dip Switch", "Value"});
    m_tableView->setModel(m_model);
    // Form layout: the name column fills the pane, the value column is a fixed,
    // comfortable width on the right (enough for dropdowns and Time min/sec).
    m_tableView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tableView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_tableView->setColumnWidth(1, 220);
    m_tableView->verticalHeader()->setVisible(false);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_delegate = new DipSwitchDelegate(this);
    m_tableView->setItemDelegateForColumn(1, m_delegate);

    connect(m_model, &QStandardItemModel::itemChanged, this, [this](QStandardItem*) {
        if (!m_buildingTable) recordEdit();
    });

    rightLayout->addWidget(m_tableView, 1);  // table fills the available height

    QHBoxLayout* saveRow = new QHBoxLayout();
    m_saveButton = new QPushButton("Save Changes");
    m_saveButton->setEnabled(false);
    connect(m_saveButton, &QPushButton::clicked, this, &MainWindow::saveFile);
    saveRow->addWidget(m_saveButton);

    m_resetButton = new QPushButton("Reset to Defaults");
    m_resetButton->setEnabled(false);
    m_resetButton->setToolTip("Restore this title's factory default settings from its P-ROM");
    connect(m_resetButton, &QPushButton::clicked, this, &MainWindow::resetToDefaults);
    saveRow->addWidget(m_resetButton);

    m_exportButton = new QPushButton("Export…");
    m_exportButton->setEnabled(false);
    m_exportButton->setToolTip("Export this title's settings to a JSON profile");
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::exportSettings);
    saveRow->addWidget(m_exportButton);

    m_importButton = new QPushButton("Import…");
    m_importButton->setEnabled(false);
    m_importButton->setToolTip("Apply a JSON settings profile to its matching title");
    connect(m_importButton, &QPushButton::clicked, this, &MainWindow::importSettings);
    saveRow->addWidget(m_importButton);

    m_shareButton = new QPushButton("Share…");
    m_shareButton->setEnabled(false);
    m_shareButton->setToolTip("Copy this title's settings as JSON, or paste & apply someone else's");
    connect(m_shareButton, &QPushButton::clicked, this, &MainWindow::shareSettings);
    saveRow->addWidget(m_shareButton);

    saveRow->addStretch(1);
    m_changedOnlyChk = new QCheckBox("Changed only");
    m_changedOnlyChk->setToolTip("Show only settings that differ from their ROM default");
    connect(m_changedOnlyChk, &QCheckBox::toggled, this, &MainWindow::filterChangedRows);
    saveRow->addWidget(m_changedOnlyChk);
    rightLayout->addLayout(saveRow);

    QLabel* cloneHint = new QLabel(
        "Tip: check titles on the left, then Tools → Clone Settings… to "
        "copy this title's settings onto them.");
    cloneHint->setStyleSheet("font-size: 10px; color: gray;");
    cloneHint->setWordWrap(true);
    rightLayout->addWidget(cloneHint);
    rightMainLayout->addWidget(m_rightPanel, 3);

    // Log
    m_logView = new QTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setMaximumHeight(150);
    m_logView->setPlaceholderText("Operation log...");
    m_logView->setStyleSheet("font-family: monospace; font-size: 11px;");
    rightMainLayout->addWidget(new QLabel("Log:"));
    rightMainLayout->addWidget(m_logView, 1);

    m_splitter->addWidget(rightContainer);
    m_splitter->setSizes({280, 820});
    mainLayout->addWidget(m_splitter);
    setCentralWidget(centralWidget);
    statusBar()->showMessage("Ready");
}

void MainWindow::setupMenuBar() {
    QMenu* file = menuBar()->addMenu("&File");
    QAction* openFile = file->addAction("&Open File...");
    openFile->setShortcut(QKeySequence::Open);
    connect(openFile, &QAction::triggered, this, &MainWindow::openFile);

    QAction* openDir = file->addAction("Open &Folder...");
    openDir->setShortcut(QKeySequence("Ctrl+D"));
    connect(openDir, &QAction::triggered, this, &MainWindow::openDirectory);

    QAction* save = file->addAction("&Save");
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, &MainWindow::saveFile);

    file->addSeparator();
    QAction* settings = file->addAction("&Settings…");
    settings->setShortcut(QKeySequence("Ctrl+,"));
    connect(settings, &QAction::triggered, this, &MainWindow::openSettings);

    file->addSeparator();
    QAction* exit = file->addAction("E&xit");
    exit->setShortcut(QKeySequence::Quit);
    connect(exit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu("&Edit");
    QAction* undoAct = edit->addAction("&Undo");
    undoAct->setShortcut(QKeySequence::Undo);
    connect(undoAct, &QAction::triggered, this, &MainWindow::undo);
    QAction* redoAct = edit->addAction("&Redo");
    redoAct->setShortcut(QKeySequence::Redo);
    connect(redoAct, &QAction::triggered, this, &MainWindow::redo);

    QMenu* tools = menuBar()->addMenu("&Tools");
    QAction* clone = tools->addAction("&Clone Settings to Selected Titles…");
    clone->setShortcut(QKeySequence("Ctrl+L"));
    connect(clone, &QAction::triggered, this, &MainWindow::cloneSettings);
    QAction* setAcross = tools->addAction("Set a Setting &Across Titles…");
    connect(setAcross, &QAction::triggered, this, &MainWindow::setSettingAcrossTitles);
    tools->addSeparator();
    QAction* bulkExp = tools->addAction("Export All Settings (collection)…");
    connect(bulkExp, &QAction::triggered, this, &MainWindow::bulkExportSettings);
    QAction* bulkImp = tools->addAction("Import Settings Collection…");
    connect(bulkImp, &QAction::triggered, this, &MainWindow::bulkImportSettings);
    tools->addSeparator();
    QAction* backup = tools->addAction("&Backup All Settings…");
    connect(backup, &QAction::triggered, this, &MainWindow::backupAllSettings);
    QAction* restore = tools->addAction("&Restore from Backup…");
    connect(restore, &QAction::triggered, this, &MainWindow::restoreFromBackup);
    tools->addSeparator();
    QAction* genAll = tools->addAction("Generate .softdips for &All Titles…");
    connect(genAll, &QAction::triggered, this, &MainWindow::generateAllSoftdips);
    QAction* genSel = tools->addAction("Generate .softdips for &Selected Titles…");
    connect(genSel, &QAction::triggered, this, &MainWindow::generateSelectedSoftdips);
    tools->addSeparator();
    QAction* audit = tools->addAction("&Audit Titles vs P-ROM…");
    connect(audit, &QAction::triggered, this, &MainWindow::auditTitles);

    QMenu* help = menuBar()->addMenu("&Help");
    QAction* about = help->addAction("&About");
    connect(about, &QAction::triggered, this, &MainWindow::about);
}

void MainWindow::openFile() {
    QString path = QFileDialog::getOpenFileName(this, "Open .softdips", QString(),
        "SoftDips (*.softdips);;All (*)");
    if (!path.isEmpty()) {
        m_games.clear();
        m_gameList->clear();
        loadFile(path.toStdString());
    }
}

void MainWindow::openDirectory() {
    QString dirPath = QFileDialog::getExistingDirectory(this, "Open Game Directory");
    if (!dirPath.isEmpty()) loadDirectory(dirPath.toStdString());
}

void MainWindow::openSettings() {
    if (!m_settingsDialog) {
        m_settingsDialog = new SettingsDialog(this);
        connect(m_settingsDialog, &SettingsDialog::openDirectoryRequested, this,
                [this](const QString& dir) { loadDirectory(dir.toStdString()); });
    }
    m_settingsDialog->reload();
    m_settingsDialog->show();
    m_settingsDialog->raise();
    m_settingsDialog->activateWindow();
}

void MainWindow::saveFile() {
    if (m_currentFilePath.empty() || !m_softDipsFile) return;

    syncTableToSource();  // pull the table's edits into m_softDipsFile

    if (softdips::SoftDipsParser::write(m_currentFilePath, *m_softDipsFile)) {
        logMessage("Saved: " + QString::fromStdString(m_currentFilePath));
        statusBar()->showMessage("Saved");
        m_saveButton->setEnabled(false);
        m_undo.clear(); m_redo.clear();
        markCurrentDirty(false);
    } else {
        logMessage("FAILED: " + QString::fromStdString(m_currentFilePath));
    }
}

void MainWindow::resetToDefaults() {
    if (m_currentGameIndex < 0 || m_currentGameIndex >= (int)m_games.size()) return;
    auto& game = m_games[m_currentGameIndex];
    if (!game.hasSoftDips || !m_softDipsFile) return;

    // Factory defaults live in the P-ROM, not the .softdips (which holds edits).
    std::string diag;
    auto defaults = softdips::SoftDipsParser::extractFromDir(game.dirPath, &diag);
    if (!defaults) {
        QMessageBox::warning(this, "Reset to Defaults",
            "Couldn't read factory defaults from this title's program ROM.\n\n" +
            QString::fromStdString(diag));
        return;
    }

    auto reply = QMessageBox::question(this, "Reset to Defaults",
        QString("Reset \"%1\" to its factory default soft DIP settings?\n\n"
                "This discards the current settings for this title.")
            .arg(QString::fromStdString(m_softDipsFile->gameName)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_softDipsFile = defaults;
    game.softDips = defaults;
    if (softdips::SoftDipsParser::write(game.filePath, *m_softDipsFile)) {
        logMessage("Reset to defaults: " + QString::fromStdString(m_softDipsFile->gameName));
        statusBar()->showMessage("Reset to defaults");
    } else {
        logMessage("Reset FAILED to save: " + QString::fromStdString(game.filePath));
    }

    updateTable();
    m_saveButton->setEnabled(false);
}

void MainWindow::about() {
    const QString name = QApplication::applicationName();
    const QString ver  = QApplication::applicationVersion();
    QMessageBox::about(this, "About " + name,
        "<center>" + name + " v" + ver + "<br>"
        "For the BackBit NeoGeo MVS Platinum Cartridge<br><br>"
        "by morb -- <a href=\"https://meson.ninja/\">"
        "https://meson.ninja/</a><br><a href=\"https://github.com/m0rb/softdips-manager\">"
        "https://github.com/m0rb/softdips-manager/</a>"
        "<br><br>Thanks to evie, HornHeaDD, NeoGeo81, lithy, and<br>" 
        "the rest of The BackBit Forum™ community</center>"  
    );
}

void MainWindow::logMessage(const QString& msg) {
    m_logView->append("[" + QDateTime::currentDateTime().toString("hh:mm:ss") + "] " + msg);
}

// ── Directory loading ──

void MainWindow::openPath(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        loadDirectory(path);
    else
        loadFile(path);
}

void MainWindow::loadFile(const std::string& filePath) {
    auto file = softdips::SoftDipsParser::parse(filePath);
    if (!file) { logMessage("ERROR: " + QString::fromStdString(filePath)); return; }
    m_currentFilePath = filePath;
    m_softDipsFile = std::move(file);
    m_undo.clear(); m_redo.clear();
    m_gameLabel->setText(QString::fromStdString(m_softDipsFile->gameName));
    m_createFromRomBtn->setEnabled(false);
    updateTable();
    updateStatusBar();
    m_saveButton->setEnabled(false);
    m_exportButton->setEnabled(true);
    m_importButton->setEnabled(true);
    m_shareButton->setEnabled(true);
    logMessage("Loaded: " + QString::fromStdString(m_softDipsFile->gameName));
}

void MainWindow::loadDirectory(const std::string& dirPath) {
    m_games.clear();
    m_gameList->clear();
    m_selectAllTitles->setEnabled(false);
    m_selectAllTitles->setCheckState(Qt::Unchecked);

    QDir dir(QString::fromStdString(dirPath));
    QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const auto& entry : entries) {
        GameEntry game;
        game.dirName = entry.fileName().toStdString();
        game.dirPath = entry.absoluteFilePath().toStdString();
        QString spath = entry.absoluteFilePath() + "/.softdips";

        if (QFileInfo::exists(spath)) {
            game.hasSoftDips = true;
            game.filePath = spath.toStdString();
            auto file = softdips::SoftDipsParser::parse(game.filePath);
            if (file) game.softDips = std::move(file);
            else game.hasSoftDips = false;
        } else {
            game.hasSoftDips = false;
        }

        // Check for P-ROMs (BackBit + MAME naming)
        bool hasRom = !softdips::SoftDipsParser::findProgramRoms(
            entry.absoluteFilePath().toStdString()).empty();

        // Always use directory name as the label (avoids duplicate name issues)
        QString text;
        if (game.hasSoftDips && game.softDips)
            text = "✓ " + entry.fileName();
        else if (hasRom)
            text = "↻ " + entry.fileName();
        else
            text = "○ " + entry.fileName();

        // Create checkbox item — never auto-checked
        QListWidgetItem* item = new QListWidgetItem(text);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        m_gameList->addItem(item);
        m_games.push_back(std::move(game));
    }

    logMessage(QString("Loaded %1 titles").arg(m_games.size()));
    statusBar()->showMessage(QString("%1 titles").arg(m_games.size()));
    m_selectAllTitles->setEnabled(!m_games.empty());

    // Remember this directory for next launch and add it to the cached list.
    QString absDir = QDir(QString::fromStdString(dirPath)).absolutePath();
    appsettings::setLastDir(absDir);
    appsettings::addRecentDir(absDir);
    if (m_settingsDialog) m_settingsDialog->reload();

    if (!m_games.empty()) m_gameList->setCurrentRow(0);
}

// ── Game selection ──

void MainWindow::onGameClicked(int row) {
    // Guard against silently discarding unsaved edits when switching titles.
    if (row != m_currentGameIndex && m_currentGameIndex >= 0 &&
        m_saveButton->isEnabled()) {
        auto reply = QMessageBox::question(this, "Unsaved Changes",
            "You have unsaved changes to the current title. Save them?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (reply == QMessageBox::Cancel) {
            QSignalBlocker block(m_gameList);  // revert selection, no re-entry
            m_gameList->setCurrentRow(m_currentGameIndex);
            return;
        }
        if (reply == QMessageBox::Save) saveFile();
        // Discard: fall through and load the new title.
    }
    selectGame(row);
}

void MainWindow::selectGame(int index) {
    if (index < 0 || index >= (int)m_games.size()) return;
    markCurrentDirty(false);          // clear the marker on the title we're leaving
    m_undo.clear(); m_redo.clear();
    m_currentGameIndex = index;
    const auto& game = m_games[index];

    if (!game.hasSoftDips || !game.softDips) {
        m_currentFilePath.clear();
        m_softDipsFile = std::nullopt;
        m_model->removeRows(0, m_model->rowCount());
        m_gameLabel->setText(QString::fromStdString(game.dirName) + " (no .softdips)");
        m_saveButton->setEnabled(false);
        m_resetButton->setEnabled(false);
        m_exportButton->setEnabled(false);
        m_importButton->setEnabled(false);
        m_shareButton->setEnabled(false);
        m_createFromRomBtn->setEnabled(true);
        updateStatusBar();
        return;
    }

    m_currentFilePath = game.filePath;
    m_softDipsFile = game.softDips;
    m_gameLabel->setText(QString::fromStdString(m_softDipsFile->gameName));
    m_createFromRomBtn->setEnabled(false);
    updateTable();
    updateStatusBar();
    m_saveButton->setEnabled(false);
    m_exportButton->setEnabled(true);
    m_importButton->setEnabled(true);
    m_shareButton->setEnabled(true);
    // Reset is available only if the P-ROM is on hand to read factory defaults.
    m_resetButton->setEnabled(
        !softdips::SoftDipsParser::findProgramRoms(game.dirPath).empty());
}

// ── Title selection ──

// Reflect how many selectable (.softdips) titles are checked in the tristate
// "Select all titles" box.
void MainWindow::updateTitlesSelectAllState() {
    int selectable = 0, checked = 0;
    for (int i = 0; i < m_gameList->count() && i < (int)m_games.size(); i++) {
        if (!m_games[i].hasSoftDips || !m_games[i].softDips) continue;
        selectable++;
        if (m_gameList->item(i)->checkState() == Qt::Checked) checked++;
    }

    QSignalBlocker block(m_selectAllTitles);
    if (selectable == 0 || checked == 0) m_selectAllTitles->setCheckState(Qt::Unchecked);
    else if (checked == selectable)      m_selectAllTitles->setCheckState(Qt::Checked);
    else                                 m_selectAllTitles->setCheckState(Qt::PartiallyChecked);
}

// ── Table ──

QWidget* MainWindow::makeTimeWidget(softdips::DipSwitch* minSw,
                                    softdips::DipSwitch* secSw) {
    auto* w = new QWidget();
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(2, 0, 2, 0);
    lay->setSpacing(4);

    auto addCombo = [&](softdips::DipSwitch* s, const char* unit) {
        auto* cb = new QComboBox();
        for (const auto& o : s->options) cb->addItem(QString::fromStdString(o.name));
        cb->setCurrentIndex(qBound(0, s->currentIndex, (int)s->options.size() - 1));
        // Update the switch live; the row maps to no model text (see m_rowSwitch).
        connect(cb, QOverload<int>::of(&QComboBox::activated), this, [this, s](int idx) {
            s->currentIndex = idx;
            recordEdit();
        });
        lay->addWidget(cb);
        lay->addWidget(new QLabel(unit));
    };
    addCombo(minSw, "min");
    if (secSw) addCombo(secSw, "sec");
    lay->addStretch(1);
    return w;
}

void MainWindow::updateTable() {
    m_buildingTable = true;  // suppress edit-recording from programmatic changes
    // Remove any composite index widgets before clearing the rows.
    for (int r = 0; r < m_model->rowCount(); r++)
        m_tableView->setIndexWidget(m_model->index(r, 1), nullptr);
    m_model->removeRows(0, m_model->rowCount());
    m_rowSwitch.clear();
    m_rowTimeSw.clear();
    if (!m_softDipsFile) { m_buildingTable = false; return; }

    auto allSwitches = m_softDipsFile->getAllSwitches();
    for (size_t i = 0; i < allSwitches.size(); ) {
        auto* sw = allSwitches[i++];

        // A Time setting's two halves (MIN + SEC) render as one row with two
        // dropdowns side by side.
        if (sw->kind == softdips::DipSwitch::Kind::Time && sw->timeField == 0) {
            softdips::DipSwitch* secSw = nullptr;
            if (i < allSwitches.size()) {
                auto* n = allSwitches[i];
                if (n->kind == softdips::DipSwitch::Kind::Time && n->timeField == 1 &&
                    n->metaByteIndex == sw->metaByteIndex)
                    secSw = n;
            }
            QString base = cleanLabel(QString::fromStdString(sw->name));
            int p = base.indexOf(" (MIN)");
            if (p >= 0) base = base.left(p);

            auto* ni = new QStandardItem(base);   ni->setEditable(false);
            auto* vi = new QStandardItem();        vi->setEditable(false);
            int row = m_model->rowCount();
            m_model->appendRow({ni, vi});
            m_rowSwitch.push_back(nullptr);  // edited live by the widget
            m_rowTimeSw.push_back({sw, secSw});
            m_tableView->setIndexWidget(m_model->index(row, 1), makeTimeWidget(sw, secSw));
            if (secSw) i++;  // consume the SEC half
            continue;
        }

        auto* ni = new QStandardItem(cleanLabel(QString::fromStdString(sw->name)));
        ni->setEditable(false);

        QString cur = (sw->currentIndex >= 0 && sw->currentIndex < (int)sw->options.size())
            ? cleanLabel(QString::fromStdString(sw->options[sw->currentIndex].name)) : "?";
        auto* vi = new QStandardItem(cur);
        vi->setEditable(true);

        QStringList opts;
        for (const auto& o : sw->options)
            opts << cleanLabel(QString::fromStdString(o.name));
        vi->setData(QVariant(opts), Qt::UserRole + 1);

        m_model->appendRow({ni, vi});
        m_rowSwitch.push_back(sw);
        m_rowTimeSw.push_back({nullptr, nullptr});
    }
    m_tableView->resizeRowsToContents();  // columns stretch (set in setupUI)
    filterChangedRows();
    m_buildingTable = false;
    m_stateBeforeEdit = selectionState();  // baseline for the next edit
}

// ── Undo / redo (selection-state snapshots) ──
QVector<int> MainWindow::selectionState() const {
    QVector<int> s;
    if (m_softDipsFile)
        for (const auto* sw : m_softDipsFile->getAllSwitches()) s.push_back(sw->currentIndex);
    return s;
}
void MainWindow::applyState(const QVector<int>& s) {
    if (!m_softDipsFile) return;
    auto sws = m_softDipsFile->getAllSwitches();
    for (int i = 0; i < (int)sws.size() && i < s.size(); i++) sws[i]->currentIndex = s[i];
}
void MainWindow::recordEdit() {
    if (m_buildingTable || !m_softDipsFile) return;
    m_undo.push_back(m_stateBeforeEdit);
    m_redo.clear();
    syncTableToSource();                  // pull a List edit into m_softDipsFile
    m_stateBeforeEdit = selectionState();
    m_saveButton->setEnabled(true);
    markCurrentDirty(true);
}
void MainWindow::undo() {
    if (m_undo.isEmpty() || !m_softDipsFile) return;
    m_redo.push_back(selectionState());
    applyState(m_undo.takeLast());
    if (m_currentGameIndex >= 0 && m_currentGameIndex < (int)m_games.size())
        m_games[m_currentGameIndex].softDips = m_softDipsFile;
    updateTable();                        // rebuilds UI + resets m_stateBeforeEdit
    bool dirty = !m_undo.isEmpty();
    m_saveButton->setEnabled(dirty);
    markCurrentDirty(dirty);
}
void MainWindow::redo() {
    if (m_redo.isEmpty() || !m_softDipsFile) return;
    m_undo.push_back(selectionState());
    applyState(m_redo.takeLast());
    if (m_currentGameIndex >= 0 && m_currentGameIndex < (int)m_games.size())
        m_games[m_currentGameIndex].softDips = m_softDipsFile;
    updateTable();
    m_saveButton->setEnabled(true);
    markCurrentDirty(true);
}
void MainWindow::markCurrentDirty(bool dirty) {
    if (m_currentGameIndex < 0 || m_currentGameIndex >= m_gameList->count()) return;
    auto* item = m_gameList->item(m_currentGameIndex);
    if (!item) return;
    QString t = item->text();
    bool has = t.endsWith(" ●");
    if (dirty && !has) item->setText(t + " ●");
    else if (!dirty && has) item->setText(t.left(t.length() - 2));
}
void MainWindow::filterTitles(const QString& query) {
    for (int i = 0; i < m_gameList->count() && i < (int)m_games.size(); i++) {
        QString hay = QString::fromStdString(m_games[i].dirName);
        if (m_games[i].softDips) hay += " " + QString::fromStdString(m_games[i].softDips->gameName);
        m_gameList->item(i)->setHidden(!query.isEmpty() && !hay.contains(query, Qt::CaseInsensitive));
    }
}

// Read the table's current selections back into m_softDipsFile (the object the
// table is built from), matching by cleaned label so simplified values still map.
// Time rows are skipped — their combos update the switches live.
void MainWindow::syncTableToSource() {
    if (!m_softDipsFile) return;
    for (int row = 0; row < m_model->rowCount() && row < m_rowSwitch.size(); row++) {
        auto* sw = m_rowSwitch[row];
        if (!sw) continue;  // Time row, handled live by its widget
        auto* vi = m_model->item(row, 1);
        if (!vi) continue;
        QString val = vi->text();
        for (size_t i = 0; i < sw->options.size(); i++) {
            if (cleanLabel(QString::fromStdString(sw->options[i].name)) == val) {
                sw->currentIndex = (int)i;
                break;
            }
        }
    }
    // Keep the game list's copy in sync so clone apply / audit see the edits.
    if (m_currentGameIndex >= 0 && m_currentGameIndex < (int)m_games.size())
        m_games[m_currentGameIndex].softDips = m_softDipsFile;
}

void MainWindow::updateStatusBar() {
    if (m_softDipsFile)
        statusBar()->showMessage(QString::fromStdString(m_softDipsFile->gameName));
    else
        statusBar()->showMessage("Ready");
}

// ── Clone settings (Tools dialog) ──

void MainWindow::cloneSettings() {
    if (!m_softDipsFile) {
        QMessageBox::information(this, "Clone Settings",
            "Open a title with settings to use as the source first.");
        return;
    }

    // Target titles = the checked ones that have data.
    std::vector<int> targets;
    for (int i = 0; i < m_gameList->count() && i < (int)m_games.size(); i++) {
        if (m_gameList->item(i)->checkState() == Qt::Checked &&
            m_games[i].hasSoftDips && m_games[i].softDips)
            targets.push_back(i);
    }
    if (targets.empty()) {
        QMessageBox::information(this, "Clone Settings",
            "Check one or more titles on the left to clone settings onto.");
        return;
    }

    syncTableToSource();  // clone the values currently shown in the editor
    const QString source = QString::fromStdString(m_softDipsFile->gameName);

    // ── Dialog: choose which of the source title's settings to copy ──
    QDialog dlg(this);
    dlg.setWindowTitle("Clone Settings");
    dlg.setMinimumSize(460, 480);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(
        QString("Copy settings from <b>%1</b> to these <b>%2</b> title(s):")
            .arg(source.toHtmlEscaped()).arg(targets.size())));

    auto* tgtList = new QListWidget();
    tgtList->setMaximumHeight(96);
    tgtList->setSelectionMode(QAbstractItemView::NoSelection);
    tgtList->setFocusPolicy(Qt::NoFocus);
    for (int idx : targets)
        tgtList->addItem(QString::fromStdString(m_games[idx].softDips->gameName));
    lay->addWidget(tgtList);

    lay->addWidget(new QLabel("Settings to copy:"));
    auto* selAll = new QCheckBox("Select all settings");
    selAll->setTristate(true);
    lay->addWidget(selAll);

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget();
    auto* col = new QVBoxLayout(inner);
    QVector<QCheckBox*> boxes;
    std::vector<CloneItem> items;
    for (auto* sw : m_softDipsFile->getAllSwitches()) {
        bool valid = sw->currentIndex >= 0 && sw->currentIndex < (int)sw->options.size();
        QString val = valid ? cleanLabel(QString::fromStdString(sw->options[sw->currentIndex].name)) : "?";
        auto* cb = new QCheckBox(cleanLabel(QString::fromStdString(sw->name)) + "  =  " + val);
        col->addWidget(cb);
        boxes.push_back(cb);
        items.push_back({sw->name, valid ? sw->options[sw->currentIndex].name : std::string()});
    }
    col->addStretch(1);
    scroll->setWidget(inner);
    lay->addWidget(scroll, 1);

    auto updateSel = [selAll, boxes]() {
        int c = 0; for (auto* cb : boxes) if (cb->isChecked()) c++;
        QSignalBlocker b(selAll);
        selAll->setCheckState(boxes.isEmpty() || c == 0 ? Qt::Unchecked
            : c == boxes.size() ? Qt::Checked : Qt::PartiallyChecked);
    };
    connect(selAll, &QCheckBox::clicked, &dlg, [selAll, boxes]() {
        bool on = selAll->checkState() != Qt::Unchecked;
        selAll->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        for (auto* cb : boxes) cb->setChecked(on);
    });
    for (auto* cb : boxes) connect(cb, &QCheckBox::toggled, &dlg, [updateSel]() { updateSel(); });

    auto* btnRow = new QHBoxLayout();
    auto* cancelBtn = new QPushButton("Cancel");
    auto* applyBtn = new QPushButton("Apply…");
    applyBtn->setDefault(true);
    btnRow->addStretch(1); btnRow->addWidget(cancelBtn); btnRow->addWidget(applyBtn);
    lay->addLayout(btnRow);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(applyBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted) return;

    std::vector<CloneItem> toApply;
    for (int i = 0; i < boxes.size() && i < (int)items.size(); i++)
        if (boxes[i]->isChecked()) toApply.push_back(items[i]);
    if (toApply.empty()) {
        QMessageBox::information(this, "Clone Settings", "No settings were selected to copy.");
        return;
    }
    executeClone(toApply, targets);
}

void MainWindow::executeClone(const std::vector<CloneItem>& toApply,
                              const std::vector<int>& titleIndices) {
    using Kind = softdips::CloneMatch::Kind;

    // Titles that actually have data.
    std::vector<int> dataGames;
    int noDataCount = 0;
    for (int idx : titleIndices) {
        if (m_games[idx].hasSoftDips && m_games[idx].softDips) dataGames.push_back(idx);
        else noDataCount++;
    }

    // ── PREVIEW: resolve every setting×game and tally outcomes ──
    int nConfident = 0, nAmbiguous = 0, nNotFound = 0;
    for (const auto& s : toApply) {
        for (int idx : dataGames) {
            auto m = softdips::SoftDipsParser::matchSetting(*m_games[idx].softDips, s.name, s.newValue);
            if (m.kind == Kind::Confident) nConfident++;
            else if (m.kind == Kind::Ambiguous) nAmbiguous++;
            else nNotFound++;
        }
    }

    {
        QStringList lines;
        if (nConfident > 0) lines << QString("%1 will apply automatically").arg(nConfident);
        if (nAmbiguous > 0) lines << QString("%1 need confirming (you'll be asked)").arg(nAmbiguous);
        if (nNotFound > 0) lines << QString("%1 have no matching switch (skipped)").arg(nNotFound);
        if (noDataCount > 0) lines << QString("%1 title(s) without .softdips (skipped)").arg(noDataCount);
        lines << "" << "See log for full details.";

        auto reply = QMessageBox::question(this, "Clone Apply Preview",
            QString("Apply %1 setting(s) to %2 title(s)?\n\n%3")
                .arg(toApply.size()).arg(titleIndices.size()).arg(lines.join("\n")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            logMessage("─── Clone Apply CANCELLED by user ───");
            return;
        }
    }

    // ── EXECUTE (setting-outer so a "repeat" decision carries to later titles) ──
    autoBackupIfEnabled("clone/apply");
    logMessage("─── Clone Apply ───");
    std::vector<bool> modified(m_games.size(), false);

    for (const auto& s : toApply) {
        logMessage(QString("• %1 = %2")
            .arg(QString::fromStdString(s.name), QString::fromStdString(s.newValue)));

        enum { Ask, AutoApply, SkipAll } repeatMode = Ask;

        for (int idx : dataGames) {
            auto& game = m_games[idx];
            QString gn = QString::fromStdString(game.softDips->gameName);
            auto m = softdips::SoftDipsParser::matchSetting(*game.softDips, s.name, s.newValue);

            std::string useSwitch;
            int useOpt = -1;

            if (m.kind == Kind::NotFound) {
                logMessage("   ⚠ " + gn + ": no matching switch — skipped");
                continue;
            } else if (m.kind == Kind::Confident) {
                useSwitch = m.candidates[0].switchName;
                useOpt = m.candidates[0].optionIndex;
            } else {  // Ambiguous
                if (repeatMode == SkipAll) {
                    logMessage("   ⚠ " + gn + ": skipped (repeat)");
                    continue;
                } else if (repeatMode == AutoApply) {
                    useSwitch = m.candidates[0].switchName;
                    useOpt = m.candidates[0].optionIndex;
                } else {
                    Choice ch = resolveAmbiguous(gn, QString::fromStdString(s.name),
                                                 QString::fromStdString(s.newValue), m);
                    if (ch.repeat) repeatMode = ch.skip ? SkipAll : AutoApply;
                    if (ch.skip) {
                        logMessage("   ⚠ " + gn + ": skipped by user");
                        continue;
                    }
                    useSwitch = ch.switchName;
                    useOpt = ch.optionIndex;
                }
            }

            auto* sw = game.softDips->findSwitch(useSwitch);
            if (sw && useOpt >= 0 && useOpt < (int)sw->options.size()) {
                sw->currentIndex = useOpt;
                modified[idx] = true;
                logMessage(QString("   ✓ %1: %2 = %3").arg(gn,
                    QString::fromStdString(useSwitch),
                    QString::fromStdString(sw->options[useOpt].name)));
            }
        }
    }

    // Write each modified title once.
    int saved = 0, failed = 0;
    for (int idx = 0; idx < (int)m_games.size(); idx++) {
        if (!modified[idx]) continue;
        if (softdips::SoftDipsParser::write(m_games[idx].filePath, *m_games[idx].softDips)) saved++;
        else {
            failed++;
            logMessage("   ✗ save failed: " + QString::fromStdString(m_games[idx].dirName));
        }
    }

    logMessage(QString("─── Done: %1 saved, %2 not found, %3 without data%4 ───")
        .arg(saved).arg(nNotFound).arg(noDataCount)
        .arg(failed ? QString(", %1 save error(s)").arg(failed) : QString()));

    if (m_currentGameIndex >= 0) selectGame(m_currentGameIndex);
}

// ── Settings profiles (export / import / bulk) ──
// Profile JSON (shared with the web app):
//   { app, v:1, gameName, settings:[{name,value}] }

MainWindow::Choice MainWindow::resolveAmbiguous(const QString& gameName, const QString& conceptName,
                                                const QString& desired, const softdips::CloneMatch& m) {
    QDialog dlg(this);
    dlg.setWindowTitle("Confirm Setting");
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(
        QString("<b>%1</b><br>Set <b>%2</b> to <b>%3</b> — choose how to apply it:")
            .arg(gameName.toHtmlEscaped(), conceptName.toHtmlEscaped(), desired.toHtmlEscaped())));
    auto* combo = new QComboBox();
    for (const auto& c : m.candidates)
        combo->addItem(QString::fromStdString(c.switchName) + "  →  " +
                       QString::fromStdString(c.optionName));
    lay->addWidget(combo);
    auto* repeatChk = new QCheckBox("Apply this decision to all remaining titles for this setting");
    lay->addWidget(repeatChk);
    auto* row = new QHBoxLayout();
    auto* skipBtn = new QPushButton("Skip");
    auto* applyBtn = new QPushButton("Apply");
    applyBtn->setDefault(true);
    row->addStretch(1); row->addWidget(skipBtn); row->addWidget(applyBtn);
    lay->addLayout(row);

    Choice ch;
    connect(applyBtn, &QPushButton::clicked, &dlg, [&]() { ch.skip = false; dlg.accept(); });
    connect(skipBtn,  &QPushButton::clicked, &dlg, [&]() { ch.skip = true;  dlg.accept(); });
    dlg.exec();

    ch.repeat = repeatChk->isChecked();
    if (!ch.skip) {
        int i = combo->currentIndex();
        if (i >= 0 && i < (int)m.candidates.size()) {
            ch.switchName = m.candidates[i].switchName;
            ch.optionIndex = m.candidates[i].optionIndex;
        }
    }
    return ch;
}

std::vector<MainWindow::CloneItem> MainWindow::settingsOf(const softdips::SoftDipsFile& file) const {
    std::vector<CloneItem> items;
    for (const auto* sw : file.getAllSwitches())
        if (sw->currentIndex >= 0 && sw->currentIndex < (int)sw->options.size())
            items.push_back({sw->name, sw->options[sw->currentIndex].name});
    return items;
}

int MainWindow::applyProfileToFile(const std::vector<CloneItem>& items,
                                   softdips::SoftDipsFile& file, const QString& label) {
    using Kind = softdips::CloneMatch::Kind;
    logMessage("─── " + label + " ───");
    int applied = 0;
    for (const auto& s : items) {
        auto m = softdips::SoftDipsParser::matchSetting(file, s.name, s.newValue);
        std::string useSwitch; int useOpt = -1;
        if (m.kind == Kind::NotFound) {
            logMessage("   ⚠ " + QString::fromStdString(s.name) + ": no match — skipped");
            continue;
        } else if (m.kind == Kind::Confident) {
            useSwitch = m.candidates[0].switchName; useOpt = m.candidates[0].optionIndex;
        } else {
            Choice ch = resolveAmbiguous(QString::fromStdString(file.gameName),
                                         QString::fromStdString(s.name),
                                         QString::fromStdString(s.newValue), m);
            if (ch.skip) { logMessage("   ⚠ " + QString::fromStdString(s.name) + ": skipped"); continue; }
            useSwitch = ch.switchName; useOpt = ch.optionIndex;
        }
        auto* sw = file.findSwitch(useSwitch);
        if (sw && useOpt >= 0 && useOpt < (int)sw->options.size()) {
            sw->currentIndex = useOpt; applied++;
            logMessage("   ✓ " + QString::fromStdString(sw->name) + " = " +
                       QString::fromStdString(sw->options[useOpt].name));
        }
    }
    return applied;
}

QString MainWindow::profileJson(const softdips::SoftDipsFile& file, bool compact) const {
    QJsonArray arr;
    for (const auto& it : settingsOf(file)) {
        QJsonObject o;
        o["name"]  = QString::fromStdString(it.name);
        o["value"] = QString::fromStdString(it.newValue);
        arr.append(o);
    }
    QJsonObject root;
    root["app"] = "softdips-manager";
    root["v"] = 1;
    root["gameName"] = QString::fromStdString(file.gameName);
    root["settings"] = arr;
    return QString::fromUtf8(QJsonDocument(root).toJson(
        compact ? QJsonDocument::Compact : QJsonDocument::Indented));
}

void MainWindow::exportSettings() {
    if (!m_softDipsFile) {
        QMessageBox::information(this, "Export Settings", "Open a title first.");
        return;
    }
    syncTableToSource();
    const QString json = profileJson(*m_softDipsFile);

    QString safe;
    for (QChar c : QString::fromStdString(m_softDipsFile->gameName).simplified())
        safe += (c.isLetterOrNumber() || c == ' ' || c == '.' || c == '_' || c == '-') ? c : QChar('_');
    if (safe.trimmed().isEmpty()) safe = "settings";

    QString path = QFileDialog::getSaveFileName(this, "Export Settings",
        safe + ".softdips.json", "Settings JSON (*.json)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Export Settings", "Could not write file.");
        return;
    }
    f.write(json.toUtf8());
    logMessage("Exported settings: " + path);
}

void MainWindow::shareSettings() {
    if (!m_softDipsFile) return;
    syncTableToSource();
    const QString code = QString::fromUtf8(profileJson(*m_softDipsFile, true).toUtf8()
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    QDialog dlg(this);
    dlg.setWindowTitle("Share Settings");
    dlg.setMinimumWidth(440);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel("A share code for this title's settings. Copy it, or paste a code "
                              "(or raw JSON) here and Apply:"));
    auto* text = new QPlainTextEdit(code);
    lay->addWidget(text);

    auto* row = new QHBoxLayout();
    auto* copyBtn = new QPushButton("Copy");
    auto* applyBtn = new QPushButton("Apply");
    auto* closeBtn = new QPushButton("Close");
    row->addWidget(copyBtn); row->addWidget(applyBtn); row->addStretch(1); row->addWidget(closeBtn);
    lay->addLayout(row);

    connect(copyBtn, &QPushButton::clicked, &dlg, [text]() { QApplication::clipboard()->setText(text->toPlainText()); });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(applyBtn, &QPushButton::clicked, &dlg, [this, &dlg, text]() {
        // Accept a base64 share code, a web share link (…#s=CODE), or raw JSON.
        QString in = text->toPlainText().trimmed();
        int h = in.indexOf("#s=");
        if (h >= 0) in = in.mid(h + 3).trimmed();
        QByteArray jsonBytes = in.startsWith('{') ? in.toUtf8()
            : QByteArray::fromBase64(in.toUtf8(), QByteArray::Base64UrlEncoding);

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            QMessageBox::warning(&dlg, "Share Settings", "That isn't a valid share code or profile.");
            return;
        }
        std::vector<CloneItem> items;
        for (const auto& v : doc.object().value("settings").toArray()) {
            QJsonObject o = v.toObject();
            if (o.contains("name") && o.contains("value"))
                items.push_back({o["name"].toString().toStdString(), o["value"].toString().toStdString()});
        }
        if (items.empty()) { QMessageBox::warning(&dlg, "Share Settings", "No settings found."); return; }
        dlg.accept();
        applyImportedProfile(items, doc.object().value("gameName").toString().trimmed());
    });
    dlg.exec();
}

void MainWindow::filterChangedRows() {
    const bool on = m_changedOnlyChk && m_changedOnlyChk->isChecked();
    for (int row = 0; row < m_model->rowCount(); row++) {
        bool changed = false;
        if (row < m_rowSwitch.size() && m_rowSwitch[row]) {
            auto* sw = m_rowSwitch[row];
            changed = sw->currentIndex != sw->defaultIndex;
        } else if (row < m_rowTimeSw.size() && m_rowTimeSw[row].first) {
            auto* mn = m_rowTimeSw[row].first;
            auto* sc = m_rowTimeSw[row].second;
            changed = (mn->currentIndex != mn->defaultIndex) ||
                      (sc && sc->currentIndex != sc->defaultIndex);
        }
        m_tableView->setRowHidden(row, on && !changed);
    }
}

void MainWindow::importSettings() {
    if (!m_softDipsFile) {
        QMessageBox::information(this, "Import Settings", "Open a title to import into first.");
        return;
    }
    QString path = QFileDialog::getOpenFileName(this, "Import Settings", QString(),
        "Settings JSON (*.json);;All (*)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Import Settings", "Could not read file.");
        return;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this, "Import Settings", "Not a valid settings profile.");
        return;
    }
    std::vector<CloneItem> items;
    for (const auto& v : doc.object().value("settings").toArray()) {
        QJsonObject o = v.toObject();
        if (o.contains("name") && o.contains("value"))
            items.push_back({o["name"].toString().toStdString(),
                             o["value"].toString().toStdString()});
    }
    if (items.empty()) {
        QMessageBox::warning(this, "Import Settings", "Profile has no settings.");
        return;
    }

    applyImportedProfile(items, doc.object().value("gameName").toString().trimmed());
}

void MainWindow::applyImportedProfile(const std::vector<CloneItem>& items, const QString& wantName) {
    if (items.empty()) return;
    auto sameName = [](const QString& a, const QString& b) {
        return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
    };

    // Folder mode: find the title these settings are for and offer to switch to it.
    if (!m_games.empty()) {
        int match = -1;
        for (int i = 0; i < (int)m_games.size(); i++)
            if (m_games[i].hasSoftDips && m_games[i].softDips &&
                sameName(QString::fromStdString(m_games[i].softDips->gameName), wantName)) { match = i; break; }

        if (match >= 0 && match != m_currentGameIndex) {
            auto r = QMessageBox::question(this, "Apply Settings",
                QString("These settings are for \"%1\". Switch to that title and apply?")
                    .arg(QString::fromStdString(m_games[match].softDips->gameName)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (r != QMessageBox::Yes) return;
            m_gameList->setCurrentRow(match);          // selectGame via signal (unsaved guard)
            if (m_currentGameIndex != match) return;   // switch cancelled
        } else if (match < 0) {
            if (!m_softDipsFile) { QMessageBox::information(this, "Apply Settings", "Open a title first."); return; }
            auto r = QMessageBox::question(this, "Apply Settings",
                QString("These settings are for \"%1\", which isn't in this folder.\n\n"
                        "Apply to the current title \"%2\" anyway?")
                    .arg(wantName.isEmpty() ? QString("(unknown)") : wantName,
                         QString::fromStdString(m_softDipsFile->gameName)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (r != QMessageBox::Yes) return;
        }
        syncTableToSource();
        executeClone(items, { m_currentGameIndex });
        return;
    }

    // Single-file mode.
    if (!m_softDipsFile) return;
    if (!wantName.isEmpty() && !sameName(QString::fromStdString(m_softDipsFile->gameName), wantName)) {
        auto r = QMessageBox::question(this, "Apply Settings",
            QString("These settings are for \"%1\" but the open file is \"%2\".\n\nApply anyway?")
                .arg(wantName, QString::fromStdString(m_softDipsFile->gameName)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) return;
    }
    syncTableToSource();
    int n = applyProfileToFile(items, *m_softDipsFile, "Import");
    if (n > 0) {
        updateTable();
        m_saveButton->setEnabled(true);
        logMessage(QString("Applied %1 setting(s) — review and Save.").arg(n));
    } else {
        QMessageBox::information(this, "Apply Settings", "No settings matched this title.");
    }
}

void MainWindow::bulkExportSettings() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Export All Settings", "Open a folder first.");
        return;
    }
    syncTableToSource();
    QJsonArray titles;
    for (const auto& g : m_games) {
        if (!g.hasSoftDips || !g.softDips) continue;
        QJsonArray arr;
        for (const auto& it : settingsOf(*g.softDips)) {
            QJsonObject o;
            o["name"]  = QString::fromStdString(it.name);
            o["value"] = QString::fromStdString(it.newValue);
            arr.append(o);
        }
        QJsonObject t;
        t["gameName"] = QString::fromStdString(g.softDips->gameName);
        t["dir"]      = QString::fromStdString(g.dirName);
        t["settings"] = arr;
        titles.append(t);
    }
    if (titles.isEmpty()) {
        QMessageBox::information(this, "Export All Settings", "No titles with settings to export.");
        return;
    }
    QJsonObject root;
    root["app"] = "softdips-manager";
    root["v"] = 1;
    root["type"] = "collection";
    root["titles"] = titles;

    QString path = QFileDialog::getSaveFileName(this, "Export All Settings",
        "collection.softdips-collection.json", "Settings collection (*.json)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Export All Settings", "Could not write file.");
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    logMessage(QString("Bulk exported %1 title(s).").arg(titles.size()));
}

void MainWindow::bulkImportSettings() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Import Settings Collection", "Open a folder first.");
        return;
    }
    QString path = QFileDialog::getOpenFileName(this, "Import Settings Collection", QString(),
        "Settings collection (*.json);;All (*)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Import Settings Collection", "Could not read file.");
        return;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject() ||
        !doc.object().value("titles").isArray()) {
        QMessageBox::warning(this, "Import Settings Collection", "Not a collection file.");
        return;
    }
    auto sameName = [](const QString& a, const QString& b) {
        return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
    };

    // Match each entry to a loaded title (by dir, then game name).
    struct Job { int idx; std::vector<CloneItem> items; };
    std::vector<Job> jobs; int unmatched = 0;
    for (const auto& tv : doc.object().value("titles").toArray()) {
        QJsonObject t = tv.toObject();
        std::vector<CloneItem> items;
        for (const auto& v : t.value("settings").toArray()) {
            QJsonObject o = v.toObject();
            if (o.contains("name") && o.contains("value"))
                items.push_back({o["name"].toString().toStdString(), o["value"].toString().toStdString()});
        }
        if (items.empty()) continue;
        QString dir = t.value("dir").toString();
        QString gn  = t.value("gameName").toString();
        int idx = -1;
        for (int i = 0; i < (int)m_games.size() && idx < 0; i++)
            if (m_games[i].hasSoftDips && m_games[i].softDips &&
                QString::fromStdString(m_games[i].dirName) == dir) idx = i;
        for (int i = 0; i < (int)m_games.size() && idx < 0; i++)
            if (m_games[i].hasSoftDips && m_games[i].softDips &&
                sameName(QString::fromStdString(m_games[i].softDips->gameName), gn)) idx = i;
        if (idx >= 0) jobs.push_back({idx, std::move(items)});
        else unmatched++;
    }
    if (jobs.empty()) {
        QMessageBox::information(this, "Import Settings Collection",
            "None of the titles in this file match the open folder.");
        return;
    }
    auto r = QMessageBox::question(this, "Import Settings Collection",
        QString("Apply settings to %1 matched title(s)?%2\n\nAmbiguous matches will prompt.")
            .arg(jobs.size())
            .arg(unmatched ? QString("\n%1 title(s) had no match (skipped).").arg(unmatched) : QString()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) return;

    autoBackupIfEnabled("bulk import");
    logMessage("─── Bulk Import ───");
    int saved = 0, failed = 0;
    for (const auto& j : jobs) {
        auto& g = m_games[j.idx];
        if (applyProfileToFile(j.items, *g.softDips, QString::fromStdString(g.softDips->gameName)) <= 0) continue;
        if (softdips::SoftDipsParser::write(g.filePath, *g.softDips)) saved++;
        else { failed++; logMessage("   ✗ save failed: " + QString::fromStdString(g.dirName)); }
    }
    logMessage(QString("─── Bulk Import done: %1 saved%2 ───")
        .arg(saved).arg(failed ? QString(", %1 error(s)").arg(failed) : QString()));
    if (m_currentGameIndex >= 0) selectGame(m_currentGameIndex);
    QMessageBox::information(this, "Import Settings Collection",
        QString("Updated %1 title(s).").arg(saved));
}

// ── Backup / restore (off-card; to the configured backup folder) ──

int MainWindow::backupToDir(const QString& destRoot) {
    int n = 0;
    for (const auto& g : m_games) {
        if (!g.hasSoftDips || g.filePath.empty()) continue;
        QString d = destRoot + "/" + QString::fromStdString(g.dirName);
        QDir().mkpath(d);
        QString dst = d + "/.softdips";
        QFile::remove(dst);
        if (QFile::copy(QString::fromStdString(g.filePath), dst)) n++;
    }
    return n;
}

void MainWindow::autoBackupIfEnabled(const QString& reason) {
    if (!appsettings::autoBackup() || m_games.empty()) return;
    QString root = appsettings::backupDir() + "/auto-" +
                   QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    int n = backupToDir(root);
    if (n > 0) logMessage(QString("Auto-backup (%1): %2 title(s) → %3").arg(reason).arg(n).arg(root));
}

void MainWindow::backupAllSettings() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Backup", "Open a folder first.");
        return;
    }
    QString root = appsettings::backupDir() + "/" +
                   QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    int n = backupToDir(root);
    if (n > 0) {
        logMessage(QString("Backed up %1 title(s) → %2").arg(n).arg(root));
        QMessageBox::information(this, "Backup",
            QString("Backed up %1 title(s) to:\n%2").arg(n).arg(root));
    } else {
        QMessageBox::information(this, "Backup", "No .softdips files to back up.");
    }
}

void MainWindow::restoreFromBackup() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Restore", "Open the folder to restore into first.");
        return;
    }
    QString root = QFileDialog::getExistingDirectory(this, "Restore from Backup",
                                                     appsettings::backupDir());
    if (root.isEmpty()) return;

    std::vector<int> toRestore;
    for (int i = 0; i < (int)m_games.size(); i++)
        if (QFile::exists(root + "/" + QString::fromStdString(m_games[i].dirName) + "/.softdips"))
            toRestore.push_back(i);
    if (toRestore.empty()) {
        QMessageBox::information(this, "Restore", "No backups in that folder match this collection.");
        return;
    }
    auto r = QMessageBox::question(this, "Restore Backup",
        QString("Overwrite .softdips for %1 title(s) from the backup?\n\n"
                "This replaces their current settings.").arg(toRestore.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) return;

    logMessage("─── Restore Backup ───");
    int restored = 0;
    for (int i : toRestore) {
        auto& g = m_games[i];
        QString src = root + "/" + QString::fromStdString(g.dirName) + "/.softdips";
        QString dst = g.filePath.empty() ? QString::fromStdString(g.dirPath) + "/.softdips"
                                         : QString::fromStdString(g.filePath);
        QFile::remove(dst);
        if (!QFile::copy(src, dst)) { logMessage("  ✗ " + QString::fromStdString(g.dirName) + ": copy failed"); continue; }
        g.filePath = dst.toStdString();
        g.hasSoftDips = true;
        if (auto parsed = softdips::SoftDipsParser::parse(g.filePath)) g.softDips = parsed;
        if (auto* item = m_gameList->item(i)) item->setText("✓ " + QString::fromStdString(g.dirName));
        restored++;
        logMessage("  ✓ " + QString::fromStdString(g.dirName));
    }
    if (m_currentGameIndex >= 0) selectGame(m_currentGameIndex);
    logMessage(QString("─── Restore done: %1 restored ───").arg(restored));
    QMessageBox::information(this, "Restore", QString("Restored %1 title(s).").arg(restored));
}

void MainWindow::setSettingAcrossTitles() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Set a Setting Across Titles", "Open a folder first.");
        return;
    }
    // Catalog: setting name → values seen across loaded titles.
    std::vector<int> targets;
    std::map<QString, std::set<QString>> catalog;
    for (int i = 0; i < (int)m_games.size(); i++) {
        if (!m_games[i].hasSoftDips || !m_games[i].softDips) continue;
        targets.push_back(i);
        for (const auto* sw : m_games[i].softDips->getAllSwitches()) {
            auto& vals = catalog[cleanLabel(QString::fromStdString(sw->name))];
            for (const auto& o : sw->options) vals.insert(cleanLabel(QString::fromStdString(o.name)));
        }
    }
    if (catalog.empty()) {
        QMessageBox::information(this, "Set a Setting Across Titles", "No titles with settings are loaded.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Set a Setting Across Titles");
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(QString("Apply one setting to all %1 title(s) that have it:")
                                  .arg(targets.size())));
    auto* nameCombo = new QComboBox();
    for (const auto& kv : catalog) nameCombo->addItem(kv.first);
    lay->addWidget(nameCombo);
    auto* valCombo = new QComboBox();
    lay->addWidget(valCombo);
    auto fillVals = [&]() {
        valCombo->clear();
        for (const auto& v : catalog[nameCombo->currentText()]) valCombo->addItem(v);
    };
    connect(nameCombo, &QComboBox::currentTextChanged, &dlg, [&](const QString&) { fillVals(); });
    fillVals();

    auto* row = new QHBoxLayout();
    auto* cancelBtn = new QPushButton("Cancel");
    auto* applyBtn = new QPushButton("Apply…");
    applyBtn->setDefault(true);
    row->addStretch(1); row->addWidget(cancelBtn); row->addWidget(applyBtn);
    lay->addLayout(row);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(applyBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    if (dlg.exec() != QDialog::Accepted) return;

    std::vector<CloneItem> items = {{ nameCombo->currentText().toStdString(),
                                      valCombo->currentText().toStdString() }};
    executeClone(items, targets);
}

// ── P-ROM extraction ──

void MainWindow::createFromRom() {
    if (m_currentGameIndex < 0 || m_currentGameIndex >= (int)m_games.size()) return;
    const auto& game = m_games[m_currentGameIndex];
    if (game.hasSoftDips) return;

    auto roms = softdips::SoftDipsParser::findProgramRoms(game.dirPath);

    logMessage(QString("Scanning %1 for P-ROMs...").arg(QString::fromStdString(game.dirName)));

    for (const auto& rom : roms) {
        logMessage("  Trying: " + QString::fromStdString(rom.filename().string()));
        std::string diag;
        auto extracted = softdips::SoftDipsParser::extractFromRom(rom, &diag);
        if (!diag.empty()) {
            logMessage("    " + QString::fromStdString(diag));
        }
        if (extracted && !extracted->sections.empty()) {
            std::string outPath = game.dirPath + "/.softdips";
            if (softdips::SoftDipsParser::write(outPath, *extracted)) {
                logMessage("  ✓ Created .softdips");

                auto& g = m_games[m_currentGameIndex];
                g.hasSoftDips = true;
                g.filePath = outPath;
                g.softDips = std::move(extracted);

                if (auto* item = m_gameList->item(m_currentGameIndex)) {
                    item->setText("✓ " + QString::fromStdString(g.dirName));
                    // Leave the clone-selection checkbox unchecked; creating a
                    // .softdips shouldn't opt the title into clone apply.
                    item->setCheckState(Qt::Unchecked);
                }

                selectGame(m_currentGameIndex);
                return;
            }
        }
    }

    logMessage("  No soft DIP settings found in this game's P-ROM (it may have none).");
    QMessageBox::information(this, "No Soft DIPs",
        "No soft DIP settings were found in this game's program ROM.\n\n"
        "The game may simply have none (this is common for homebrew and\n"
        "early titles). See log for details.");
}

// ── Audit ──

void MainWindow::generateAllSoftdips() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Generate", "Open a directory first.");
        return;
    }
    std::vector<int> all;
    for (int i = 0; i < (int)m_games.size(); i++) all.push_back(i);
    generateSoftdipsFor(all, "title(s)");
}

void MainWindow::generateSelectedSoftdips() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Generate", "Open a directory first.");
        return;
    }
    std::vector<int> selected;
    for (int i = 0; i < m_gameList->count() && i < (int)m_games.size(); i++)
        if (m_gameList->item(i)->checkState() == Qt::Checked) selected.push_back(i);
    if (selected.empty()) {
        QMessageBox::information(this, "Generate",
            "Check one or more titles on the left first.");
        return;
    }
    generateSoftdipsFor(selected, "selected title(s)");
}

void MainWindow::generateSoftdipsFor(const std::vector<int>& indices, const QString& noun) {
    // Candidates: titles without a .softdips that have a program ROM.
    int candidates = 0;
    for (int i : indices)
        if (!m_games[i].hasSoftDips &&
            !softdips::SoftDipsParser::findProgramRoms(m_games[i].dirPath).empty())
            candidates++;

    if (candidates == 0) {
        QMessageBox::information(this, "Generate",
            QString("None of the %1 need a .softdips generated.\n\n"
                    "(Existing files are left untouched. Use Audit to refresh stale ones.)")
                .arg(noun));
        return;
    }

    auto reply = QMessageBox::question(this, "Generate .softdips",
        QString("Create .softdips for %1 %2 that don't have one yet?\n\n"
                "Existing files are left untouched. Titles with no soft DIPs "
                "(demos) are skipped.").arg(candidates).arg(noun),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    logMessage("─── Generate .softdips ───");
    int created = 0, skipped = 0, failed = 0;
    for (int i : indices) {
        auto& g = m_games[i];
        if (g.hasSoftDips) continue;

        std::string diag;
        auto extracted = softdips::SoftDipsParser::extractFromDir(g.dirPath, &diag);
        if (!extracted) {
            if (!softdips::SoftDipsParser::findProgramRoms(g.dirPath).empty()) skipped++;
            continue;  // no ROM, or a red-herring demo with no soft DIPs
        }
        std::string outPath = g.dirPath + "/.softdips";
        if (!softdips::SoftDipsParser::write(outPath, *extracted)) {
            failed++;
            logMessage("  ✗ " + QString::fromStdString(g.dirName) + ": write failed");
            continue;
        }
        g.hasSoftDips = true;
        g.filePath = outPath;
        g.softDips = std::move(extracted);
        if (auto* item = m_gameList->item(i))
            item->setText("✓ " + QString::fromStdString(g.dirName));
        created++;
        logMessage("  ✓ " + QString::fromStdString(g.dirName));
    }

    if (m_currentGameIndex >= 0) selectGame(m_currentGameIndex);
    logMessage(QString("─── Created %1, skipped %2 (no soft DIPs)%3 ───")
        .arg(created).arg(skipped)
        .arg(failed ? QString(", %1 failed").arg(failed) : QString()));
    QMessageBox::information(this, "Generate Complete",
        QString("Created %1 .softdips file(s).\n%2 title(s) had no soft DIPs (skipped).")
            .arg(created).arg(skipped));
}

void MainWindow::auditTitles() {
    if (m_games.empty()) {
        QMessageBox::information(this, "Audit", "Open a directory first.");
        return;
    }

    using Status = softdips::AuditResult::Status;
    logMessage("─── Audit .softdips vs P-ROM ───");

    int okCount = 0, problemCount = 0, notVerified = 0;
    std::vector<int> regenerable;  // stale games that have a P-ROM to rebuild from

    for (int i = 0; i < (int)m_games.size(); i++) {
        auto& g = m_games[i];
        auto r = softdips::SoftDipsParser::auditGameDir(g.dirPath);

        // Skip directories that are neither games nor carry a .softdips.
        if (r.status == Status::NoSoftDips && r.gameName.empty()) continue;

        QString label = QString::fromStdString(g.dirName);
        QListWidgetItem* item = m_gameList->item(i);

        if (r.status == Status::Ok) {
            okCount++;
            logMessage("  ✓ " + label + " — OK");
            if (item) item->setToolTip("Audit: matches P-ROM table");
        } else if (r.isProblem()) {
            problemCount++;
            logMessage("  ✗ " + label + " — " + r.statusText());
            QStringList tip{QString("Audit: ") + r.statusText()};
            for (const auto& d : r.differences) {
                logMessage("       - " + QString::fromStdString(d));
                tip << QString::fromStdString(d);
            }
            if (item) {
                item->setToolTip(tip.join("\n"));
                if (!item->text().startsWith("⚠"))
                    item->setText("⚠ " + item->text());
            }
            if (!softdips::SoftDipsParser::findProgramRoms(g.dirPath).empty())
                regenerable.push_back(i);
        } else {
            notVerified++;
            logMessage("  · " + label + " — " + r.statusText());
            if (item) item->setToolTip(QString("Audit: ") + r.statusText());
        }
    }

    logMessage(QString("─── %1 OK, %2 problem(s), %3 not verified ───")
                   .arg(okCount).arg(problemCount).arg(notVerified));

    QString summary = QString("%1 OK\n%2 problem(s)\n%3 not verified")
                          .arg(okCount).arg(problemCount).arg(notVerified);

    if (regenerable.empty()) {
        QMessageBox::information(this, "Audit Complete", summary +
            "\n\nSee log for details.");
        return;
    }

    auto reply = QMessageBox::question(this, "Audit Complete",
        summary + QString("\n\n%1 problem title(s) can be regenerated from their "
                          "P-ROM. Regenerate now?\n\n(This overwrites those "
                          ".softdips files with fresh data from the P-ROM.)")
                      .arg(regenerable.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    autoBackupIfEnabled("audit regenerate");
    int rebuilt = 0;
    for (int i : regenerable) {
        auto& g = m_games[i];
        std::string diag;
        auto extracted = softdips::SoftDipsParser::extractFromDir(g.dirPath, &diag);
        if (!extracted) {
            logMessage("  ✗ " + QString::fromStdString(g.dirName) + ": " +
                       QString::fromStdString(diag));
            continue;
        }
        std::string outPath = g.dirPath + "/.softdips";
        if (!softdips::SoftDipsParser::write(outPath, *extracted)) {
            logMessage("  ✗ " + QString::fromStdString(g.dirName) + ": write failed");
            continue;
        }
        g.hasSoftDips = true;
        g.filePath = outPath;
        g.softDips = std::move(extracted);
        if (auto* item = m_gameList->item(i)) {
            item->setText("✓ " + QString::fromStdString(g.dirName));
            item->setToolTip("Audit: regenerated from P-ROM");
        }
        rebuilt++;
        logMessage("  ✓ Regenerated " + QString::fromStdString(g.dirName));
    }

    // Refresh the editor if the current game was regenerated.
    if (m_currentGameIndex >= 0) selectGame(m_currentGameIndex);
    logMessage(QString("─── Regenerated %1 .softdips file(s) ───").arg(rebuilt));
    QMessageBox::information(this, "Regenerated",
        QString("Regenerated %1 .softdips file(s) from their P-ROMs.").arg(rebuilt));
}
