#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QPair>
#include <QString>

#include <functional>

// Minimal ISO9660 reader (no external dependencies). Enough for PS2 game discs:
// primary volume descriptor + directory records, both-endian fields, ";" version
// suffix stripping and multi-extent files. Used by the Install Wizard to hash
// the boot ELF inside the user's disc dump and to extract the game data tree.
class Iso9660
{
public:
    struct Extent
    {
        quint32 start = 0; // in logical blocks
        quint64 size = 0;  // in bytes
    };

    struct File
    {
        QString path; // iso-style forward slashes, no leading '/', no ";1"
        bool dir = false;
        QList<Extent> extents;

        quint64 size() const
        {
            quint64 total = 0;
            for (const Extent &e : extents)
                total += e.size;
            return total;
        }
    };

    bool open(const QString &isoPath);
    bool isOpen() const { return m_opened; }
    QString error() const { return m_err; }

    const QList<File> &files() const { return m_files; }
    // Case-insensitive lookup, e.g. "SLUS_216.78" or "BIN/DBZP.BIN".
    const File *find(const QString &isoPath) const;

    // Streams the whole file in 256 KiB chunks to sink(). Returns bytes read or -1.
    qint64 readFile(const File &f, const std::function<void(const QByteArray &)> &sink) const;

private:
    bool readBlocks(quint32 startLba, quint64 byteLen, const std::function<void(const QByteArray &)> &sink) const;
    bool scanDirectory(quint32 lba, quint64 byteLen, const QString &prefix);

    QString m_path;
    bool m_opened = false;
    quint32 m_blockSize = 2048;
    QString m_err;
    QList<File> m_files;
};

// Disc-level verification for the launcher install wizard. The expected SHA-256
// below is the hash of SLUS_216.78 (US release) and MUST stay in sync with the
// ELF_SHA256 constant in games/bt3/setup.py.
namespace DiscVerify
{
constexpr const char *kExpectedDiscElfSha256 = "811188ba9b416500d921cd4d9514df0cbf42f3a41a99cf5aac5a3da37171bf99";

enum class State
{
    Missing, // data dir / boot ELF not present
    Corrupt, // present but SHA-256 mismatch
    Valid,   // SHA-256 matches
};

// Hash of the boot ELF embedded in an ISO image read with the local ISO9660 reader.
bool verifySlusFromIso(const QString &isoPath);

// Hash of the installed data/SLUS_216.78 next to the launcher.
State verifyInstalledData(const QString &dataDir);

// Variant-aware form used by the launcher. The legacy overload above remains
// the USA default for existing settings and diagnostics.
State verifyInstalledData(const QString &dataDir, const QString &bootName,
                          const QString &expectedSha256);

// Total size in bytes of all files under dataDir.
quint64 dataSize(const QString &dataDir);
} // namespace DiscVerify
