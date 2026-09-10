#pragma once

#include <QMainWindow>
#include <QString>
#include <QVector>

class QLabel;
class QComboBox;
class QPushButton;
class QProcess;
class QWidget;

class LauncherWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit LauncherWindow(QWidget *parent = nullptr);

    // Absolute path to the playable game ELF (self-extracting BT3SELFX binary)
    // found next to the launcher. Empty if none detected.
    static QString findGameElf();

private slots:
    void onPlayClicked();
    void onSettingsClicked();
    void onVariantChanged(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;

private:
    void loadBackground();
    void resolveLaunchTarget();
    void scanVariants();
    void updateHint();
    void checkGameData();
    bool openInstallWizard();

    QLabel *m_hint = nullptr;
    QComboBox *m_variant = nullptr;
    QPushButton *m_play = nullptr;
    QPushButton *m_settings = nullptr;
    QWidget *m_bottomBar = nullptr;
    QProcess *m_gameProc = nullptr;

    QString m_gameElf;
    QString m_bgPath;
    QString m_savedataDir;
    QString m_dataDir;
    struct GameVariant
    {
        QString id;
        QString title;
        QString bootName;
        QString runnerName;
        QString dataRelative;
        QString expectedSha256;
        bool selfExtracting = false;
    };
    QVector<GameVariant> m_variants;
    int m_variantIndex = 0;
    bool m_plainRunner = false;
    bool m_gameDataValid = false;
    bool m_wizardShown = false;
};
