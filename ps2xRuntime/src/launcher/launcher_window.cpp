#include "app_paths.h"
#include "launcher_window.h"

#include "dbz_theme.h"
#include "install_wizard_dialog.h"
#include "iso9660.h"
#include "settings_dialog.h"
#include "settings_manager.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

// ROM-variant table generated from games/bt3/variants/*.json by
// tools/gen_variant_table.py (see src/launcher/CMakeLists.txt). The descriptors
// are the single source of truth; nothing about a variant is spelled out here.
#include "variant_table.inc"

namespace
{
    // BT3SELFX footer: the last 32 bytes are "BT3SELFX" magic + payload info.
    bool isSelfExtractElf(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        if (f.size() < 32)
            return false;
        if (!f.seek(f.size() - 32))
            return false;
        QByteArray tail = f.read(8);
        return tail == QByteArray("BT3SELFX");
    }

    // Names the self-extracting release ELF has shipped under, newest first.
    QString selfExtractElfPath(const QDir &appDir)
    {
        static const char *kCandidates[] = {
            "Dragon Ball - Budokai Tenkaichi 3",
            "Dragon Ball Budokai Tenkaichi 3",
            "SLUS-216.78",
        };
        for (const char *c : kCandidates)
        {
            const QString p = appDir.filePath(QString::fromLatin1(c));
            if (QFile::exists(p) && isSelfExtractElf(p))
                return p;
        }
        return QString();
    }

    QString plainRunnerPath(const QDir &appDir, const QString &runnerName)
    {
#ifdef _WIN32
        return appDir.filePath(runnerName + QStringLiteral(".exe"));
#else
        return appDir.filePath(runnerName);
#endif
    }
} // namespace

LauncherWindow::LauncherWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Dragon Ball Budokai Tenkaichi 3 Launcher"));
    resize(900, 500);
    setMinimumSize(700, 420);

    // The launcher lives in the deploy root; savedata/ is the shared settings dir.
    m_savedataDir = QDir(apppaths::userRoot()).filePath(QStringLiteral("savedata"));
    m_dataDir = QDir(apppaths::userRoot()).filePath(QStringLiteral("data"));

    // USA is always the first/default profile. Alternate profiles are added
    // only when their runner is installed next to this launcher.
    scanVariants();

    // Background: deploy assets/background.png if present, else a DBZ gradient.
    const QString bg = QDir(apppaths::assets()).filePath(QStringLiteral("background.png"));
    if (QFile::exists(bg))
        m_bgPath = bg;

    // Window/taskbar icon from the same asset tree.
    const QIcon appIcon(QDir(apppaths::assets()).filePath(QStringLiteral("icon.png")));
    if (!appIcon.isNull())
        setWindowIcon(appIcon);

    // Bottom bar with PLAY (left) + SETTINGS (right).
    auto *bottomBar = new QWidget(this);
    bottomBar->setObjectName(QStringLiteral("bottomBar"));
    // Opaque bar: solid background over the image area, only a top edge line.
    bottomBar->setStyleSheet(QStringLiteral(
        "QWidget#bottomBar { background-color: #0b0f13; border-top: 1px solid #1e2830; }"));
    m_bottomBar = bottomBar;

    auto *barLayout = new QHBoxLayout(bottomBar);
    barLayout->setContentsMargins(24, 14, 24, 14);

    m_play = new QPushButton(QStringLiteral("PLAY"), bottomBar);
    m_play->setObjectName(QStringLiteral("playButton"));
    m_play->setCursor(Qt::PointingHandCursor);
    m_play->setFixedHeight(58);

    m_settings = new QPushButton(QStringLiteral("SETTINGS"), bottomBar);
    m_settings->setObjectName(QStringLiteral("settingsButton"));
    m_settings->setCursor(Qt::PointingHandCursor);
    m_settings->setFixedHeight(50);

    m_variant = new QComboBox(bottomBar);
    m_variant->setObjectName(QStringLiteral("variantSelector"));
    m_variant->setMinimumWidth(210);
    for (const GameVariant &variant : m_variants)
        m_variant->addItem(variant.title, variant.id);
    if (m_variantIndex >= 0)
        m_variant->setCurrentIndex(m_variantIndex);
    // Only one profile installed: the selector would be a one-item dropdown.
    m_variant->setVisible(m_variants.size() > 1);

    barLayout->addWidget(m_variant, 0, Qt::AlignVCenter);
    barLayout->addWidget(m_play, 0, Qt::AlignVCenter);
    barLayout->addStretch(1);
    barLayout->addWidget(m_settings, 0, Qt::AlignVCenter);

    m_hint = new QLabel(bottomBar);
    m_hint->setObjectName(QStringLiteral("hintLabel"));
    m_hint->setStyleSheet(QStringLiteral("color: #9999b3; background: transparent;"));

    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("launcherRoot"));
    // Keep the central container transparent so the background image painted
    // in paintEvent() actually shows through (the global QSS otherwise paints
    // an opaque WindowBg over it).
    root->setAttribute(Qt::WA_TranslucentBackground, true);
    root->setStyleSheet(QStringLiteral("QWidget#launcherRoot { background: transparent; }"));
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addStretch(1);
    layout->addWidget(bottomBar);
    setCentralWidget(root);

    connect(m_play, &QPushButton::clicked, this, &LauncherWindow::onPlayClicked);
    connect(m_settings, &QPushButton::clicked, this, &LauncherWindow::onSettingsClicked);
    connect(m_variant, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &LauncherWindow::onVariantChanged);

    // Slot 2: the bar is [variant, play, hint, stretch, settings]. Index 1 would
    // put the stretching hint between the selector and PLAY.
    barLayout->insertWidget(2, m_hint, 1, Qt::AlignVCenter | Qt::AlignLeft);

    checkGameData();

    // Seed settings manager from the shared savedata dir.
    SettingsManager::instance().setConfigDir(m_savedataDir);
    SettingsManager::instance().load();
}

// A profile is offered only when something can actually boot it: the
// self-extracting release ELF for the canonical target, or a plain runner binary
// next to the launcher. A deploy may therefore carry any subset -- including one
// with an alternate runner but no USA one.
//
// The selection then prefers the canonical profile whenever it is installed, and
// otherwise falls back to the first one that is. It is not persisted: every start
// re-reads what is on disk, so a profile that disappears cannot be preselected.
void LauncherWindow::scanVariants()
{
    m_variants.clear();
    const QDir appDir(QApplication::applicationDirPath());
    for (const VariantRecord &record : kVariantRecords)
    {
        GameVariant variant;
        variant.id = QString::fromLatin1(record.id);
        variant.title = QString::fromLatin1(record.label);
        variant.bootName = QString::fromLatin1(record.bootName);
        variant.runnerName = QString::fromLatin1(record.runnerName);
        variant.dataRelative = QString::fromLatin1(record.dataRelative);
        variant.expectedSha256 = QString::fromLatin1(record.expectedSha256);
        variant.selfExtracting = record.selfExtracting;

        const bool hasSelfExtract =
            variant.selfExtracting && !selfExtractElfPath(appDir).isEmpty();
        const bool hasRunner =
            QFileInfo(plainRunnerPath(appDir, variant.runnerName)).isExecutable();
        if (!hasSelfExtract && !hasRunner)
            continue;
        m_variants.push_back(variant);
    }

    m_variantIndex = m_variants.isEmpty() ? -1 : 0;
    for (int i = 0; i < m_variants.size(); ++i)
    {
        if (m_variants.at(i).id == QLatin1String(kCanonicalVariantId))
        {
            m_variantIndex = i;
            break;
        }
    }
}

void LauncherWindow::onVariantChanged(int index)
{
    if (index < 0 || index >= m_variants.size())
        return;
    m_variantIndex = index;
    m_gameElf.clear();
    m_plainRunner = false;
    checkGameData();
}

// Release deploy: no SELFX next to us, but a plain ps2EntryRunner from the
// same stage tree. Boot it directly with data/<selected boot ELF>. The USA
// profile remains the default and keeps the old self-extracting path intact.
// Re-run after the install wizard so a freshly created runner flips the hint.
void LauncherWindow::resolveLaunchTarget()
{
    m_gameElf.clear();
    m_plainRunner = false;
    if (m_variants.isEmpty() || m_variantIndex < 0 || m_variantIndex >= m_variants.size())
        return;

    const GameVariant &variant = m_variants.at(m_variantIndex);
    const QDir appDir(QApplication::applicationDirPath());
    if (variant.selfExtracting)
        m_gameElf = selfExtractElfPath(appDir);
    if (m_gameElf.isEmpty())
        m_plainRunner =
            QFileInfo(plainRunnerPath(appDir, variant.runnerName)).isExecutable();
}

void LauncherWindow::onPlayClicked()
{
    resolveLaunchTarget();
    if (!m_plainRunner && m_gameElf.isEmpty())
        return;

    if (m_variants.isEmpty() || m_variantIndex < 0 || m_variantIndex >= m_variants.size())
        return;
    const GameVariant &variant = m_variants.at(m_variantIndex);

    // Game data must be present and validated before the runner can boot.
    if (!m_gameDataValid)
    {
        // Safety net: PLAY is disabled while the data is invalid, so this is
        // only reached if the enabling logic changes. The install wizard is
        // still USA-specific; updateHint() names the directory for the rest.
        if (!variant.selfExtracting || !openInstallWizard())
            return;
    }

    // Save any pending settings so the game boots with the launcher's config.
    SettingsManager::instance().save();

    const QDir appDir(QApplication::applicationDirPath());

    QProcess *proc = new QProcess(nullptr);
    proc->setWorkingDirectory(apppaths::userRoot());
#if defined(Q_OS_MACOS)
    // Detached GUI applications do not inherit a useful terminal on macOS.
    // Keep the most recent runner diagnostics where users can attach them to a report.
    const QString logsDir = QDir(apppaths::userRoot()).filePath(QStringLiteral("logs"));
    QDir().mkpath(logsDir);
    proc->setStandardOutputFile(QDir(logsDir).filePath(QStringLiteral("game-latest.out")));
    proc->setStandardErrorFile(QDir(logsDir).filePath(QStringLiteral("game-latest.log")));
#endif

    if (m_plainRunner)
    {
        // Direct runner mode: point it at the extracted boot ELF and the
        // bundled library tree; it is already the real ps2EntryRunner.
#ifdef _WIN32
        proc->setProgram(appDir.filePath(variant.runnerName + QStringLiteral(".exe")));
#else
        proc->setProgram(appDir.filePath(variant.runnerName));
#endif
        const QString dataDir = QDir(apppaths::userRoot()).filePath(variant.dataRelative);
        proc->setArguments({QDir(dataDir).filePath(variant.bootName)});
        auto env = QProcessEnvironment::systemEnvironment();
        // [deploy] Anchor the runner's savedata/assets/fonts (and bt3_settings.ini)
        // at the deploy root -- where the launcher wrote them -- not data/.
        env.insert(QStringLiteral("PS2X_EXEDIR"), apppaths::userRoot());
        env.insert(QStringLiteral("PS2X_ASSETDIR"), apppaths::assets());
#if !defined(_WIN32) && !defined(Q_OS_MACOS)
        // position-independent loader search is a POSIX concept; Windows
        // resolves the bundled dlls from the executable's own directory, and
        // the macOS bundle resolves its dylibs through @rpath.
        env.insert(QStringLiteral("LD_LIBRARY_PATH"), appDir.filePath(QStringLiteral("lib")));
#endif
        proc->setProcessEnvironment(env);
    }
    else
    {
        // Launch detached: the game extracts + execs its own inner runner.
        proc->setProgram(m_gameElf);
    }
    if (!proc->startDetached())
    {
        QMessageBox::critical(this, QStringLiteral("Could not start the game"), proc->errorString());
        proc->deleteLater();
        return;
    }

    // The launcher's job is done: close this window (the game runs on its own).
    close();
}

void LauncherWindow::checkGameData()
{
    resolveLaunchTarget();
    if (m_variants.isEmpty() || m_variantIndex < 0 || m_variantIndex >= m_variants.size())
        m_gameDataValid = false;
    else
    {
        const GameVariant &variant = m_variants.at(m_variantIndex);
        const QString dataDir = QDir(apppaths::userRoot()).filePath(variant.dataRelative);
        m_gameDataValid = (DiscVerify::verifyInstalledData(
            dataDir, variant.bootName, variant.expectedSha256) == DiscVerify::State::Valid);
    }

    if (m_play)
        m_play->setEnabled((!m_gameElf.isEmpty() || m_plainRunner) && m_gameDataValid);

    updateHint();
}

void LauncherWindow::updateHint()
{
    const QString kRed = QStringLiteral("#ef4444");
    const QString kGreen = QStringLiteral("#22c55e");
    const QString kDim = QStringLiteral("#9999b3");

    QString color;
    QString text;
    if (m_variants.isEmpty())
    {
        color = kRed;
        text = QStringLiteral("No game runner found next to this launcher");
    }
    else if (!m_gameDataValid)
    {
        color = kRed;
        text = QStringLiteral("Missing or Corrupted Data");
        if (m_variantIndex >= 0 && m_variantIndex < m_variants.size())
        {
            const GameVariant &variant = m_variants.at(m_variantIndex);
            text = QStringLiteral("Missing or Corrupted Data — expected %1/ in %2")
                       .arg(variant.bootName,
                            QDir(apppaths::userRoot()).filePath(variant.dataRelative));
        }
    }
    else if (!m_gameElf.isEmpty() || m_plainRunner)
    {
        color = kGreen;
        text = QStringLiteral("Ready to Play");
    }
    else
    {
        color = kDim;
        text = QStringLiteral("No self-extracting game ELF found in this folder");
    }
    if (m_hint)
        m_hint->setText(QStringLiteral("<span style=\"color:%1; font-size:15px;\">●</span> %2")
                            .arg(color, text));
}

bool LauncherWindow::openInstallWizard()
{
    InstallWizardDialog dlg(this);
    const bool installed = dlg.exec() == QDialog::Accepted;
    checkGameData();
    return installed && m_gameDataValid;
}

void LauncherWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);

    // Pop the install wizard automatically on first launch when the game data is
    // missing/corrupt. It installs the canonical disc tree, so it is only offered
    // while that profile is the selected one -- an alternate profile has its own
    // data directory, which updateHint() names instead.
    const bool canonicalSelected =
        m_variantIndex >= 0 && m_variantIndex < m_variants.size() &&
        m_variants.at(m_variantIndex).id == QLatin1String(kCanonicalVariantId);
    if (!m_gameDataValid && !m_wizardShown && canonicalSelected)
    {
        m_wizardShown = true;
        QTimer::singleShot(0, this, [this] {
            if (!m_gameDataValid)
                openInstallWizard();
        });
    }
}

void LauncherWindow::onSettingsClicked()
{
    SettingsDialog dlg(this);
    dlg.exec();
}

void LauncherWindow::loadBackground() { /* bg applied in paintEvent */ }

void LauncherWindow::paintEvent(QPaintEvent *)
{
    // The background image only fills the area above the (opaque) bottom bar.
    const int barY = m_bottomBar ? m_bottomBar->y() : height();
    const QRect bgArea(0, 0, width(), barY);

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!m_bgPath.isEmpty())
    {
        QPixmap bg(m_bgPath);
        if (!bg.isNull())
        {
            p.drawPixmap(bgArea, bg.scaled(bgArea.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            return;
        }
    }
    // Fallback: dark DBZ gradient (top->bottom) within the image area.
    QLinearGradient g(0, 0, 0, bgArea.height());
    g.setColorAt(0.0, QColor(16, 22, 28));
    g.setColorAt(0.5, QColor(9, 14, 18));
    g.setColorAt(1.0, QColor(5, 8, 12));
    p.fillRect(bgArea, g);

    // Corner brackets (accent orange), matching the capsule-HUD look.
    const QColor bracket(255, 158, 26, 200);
    QPen pen(bracket, 2.0);
    p.setPen(pen);
    const int bl = 14, off = 6;
    const QRect r = bgArea.adjusted(1, 1, -1, -1);
    QPoint tl = r.topLeft(), tr = r.topRight(), blc = r.bottomLeft(), br = r.bottomRight();
    p.drawLine(tl + QPoint(off, off), tl + QPoint(off + bl, off));
    p.drawLine(tl + QPoint(off, off), tl + QPoint(off, off + bl));
    p.drawLine(tr - QPoint(off, off), tr - QPoint(off + bl, -off));
    p.drawLine(tr - QPoint(off, off), tr - QPoint(-off, off + bl));
    p.drawLine(blc + QPoint(off, -off), blc + QPoint(off + bl, -off));
    p.drawLine(blc + QPoint(off, -off), blc + QPoint(off, -off - bl));
    p.drawLine(br - QPoint(off, -off), br - QPoint(off + bl, -off));
    p.drawLine(br - QPoint(off, -off), br - QPoint(-off, off + bl));
}
