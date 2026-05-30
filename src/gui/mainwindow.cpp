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
#include <QFileInfoList>
#include <QDateTime>
#include <QTimer>
#include <QApplication>

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
    m_tableView->horizontalHeader()->setStretchLastSection(true);
    m_tableView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tableView->verticalHeader()->setVisible(false);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_delegate = new DipSwitchDelegate(this);
    m_tableView->setItemDelegateForColumn(1, m_delegate);

    connect(m_model, &QStandardItemModel::itemChanged, this, [this](QStandardItem*) {
        m_saveButton->setEnabled(true);
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

    QMenu* tools = menuBar()->addMenu("&Tools");
    QAction* clone = tools->addAction("&Clone Settings to Selected Titles…");
    clone->setShortcut(QKeySequence("Ctrl+L"));
    connect(clone, &QAction::triggered, this, &MainWindow::cloneSettings);
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
        "by morb -- <a href=\"https://meson.ninja/\">
        https://meson.ninja/</a><br><a href=\"https://github.com/m0rb/softdips-manager\">
        https://github.com/m0rb/softdips-manager</a></center>");
}

void MainWindow::logMessage(const QString& msg) {
    m_logView->append("[" + QDateTime::currentDateTime().toString("hh:mm:ss") + "] " + msg);
}

// ── Directory loading ──

void MainWindow::loadFile(const std::string& filePath) {
    auto file = softdips::SoftDipsParser::parse(filePath);
    if (!file) { logMessage("ERROR: " + QString::fromStdString(filePath)); return; }
    m_currentFilePath = filePath;
    m_softDipsFile = std::move(file);
    m_gameLabel->setText(QString::fromStdString(m_softDipsFile->gameName));
    m_createFromRomBtn->setEnabled(false);
    updateTable();
    updateStatusBar();
    m_saveButton->setEnabled(false);
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
    m_currentGameIndex = index;
    const auto& game = m_games[index];

    if (!game.hasSoftDips || !game.softDips) {
        m_currentFilePath.clear();
        m_softDipsFile = std::nullopt;
        m_model->removeRows(0, m_model->rowCount());
        m_gameLabel->setText(QString::fromStdString(game.dirName) + " (no .softdips)");
        m_saveButton->setEnabled(false);
        m_resetButton->setEnabled(false);
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
            m_saveButton->setEnabled(true);
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
    // Remove any composite index widgets before clearing the rows.
    for (int r = 0; r < m_model->rowCount(); r++)
        m_tableView->setIndexWidget(m_model->index(r, 1), nullptr);
    m_model->removeRows(0, m_model->rowCount());
    m_rowSwitch.clear();
    if (!m_softDipsFile) return;

    auto allSwitches = m_softDipsFile->getAllSwitches();
    for (size_t i = 0; i < allSwitches.size(); i++) {
        auto* sw = allSwitches[i];

        // A Time setting's two halves (MIN + SEC) render as one row with two
        // dropdowns side by side.
        if (sw->kind == softdips::DipSwitch::Kind::Time && sw->timeField == 0) {
            softdips::DipSwitch* secSw = nullptr;
            if (i + 1 < allSwitches.size()) {
                auto* n = allSwitches[i + 1];
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
    }
    m_tableView->resizeColumnsToContents();
    m_tableView->resizeRowsToContents();
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

    // Interactive resolver for ambiguous matches: lets the user pick the right
    // switch/value, skip this title, or apply the decision to all remaining
    // titles for this setting.
    struct Choice { bool skip = true; std::string switchName; int optionIndex = -1; bool repeat = false; };
    auto askResolve = [this](const QString& gameName, const QString& concept,
                             const QString& desired,
                             const softdips::CloneMatch& m) -> Choice {
        QDialog dlg(this);
        dlg.setWindowTitle("Confirm Setting");
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel(
            QString("<b>%1</b><br>Set <b>%2</b> to <b>%3</b> — choose how to apply it:")
                .arg(gameName.toHtmlEscaped(), concept.toHtmlEscaped(), desired.toHtmlEscaped())));
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
    };

    // ── EXECUTE (setting-outer so a "repeat" decision carries to later titles) ──
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
                    Choice ch = askResolve(gn, QString::fromStdString(s.name),
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

                QListWidgetItem* item = m_gameList->item(m_currentGameIndex);
                item->setText("✓ " + QString::fromStdString(g.dirName));
                // Leave the clone-selection checkbox unchecked; creating a
                // .softdips shouldn't opt the title into clone apply.
                item->setCheckState(Qt::Unchecked);

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
