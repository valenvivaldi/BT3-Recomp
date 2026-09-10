#include "iso9660.h"

#include <QChar>
#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QtEndian>

#include <optional>

namespace
{
constexpr quint32 kSectorSize = 2048;
constexpr quint64 kPvdLba = 16;

// ISO9660 directory record (34-byte base descriptor).
struct DirRecord
{
    bool valid = false;
    bool isDir = false;
    bool multiExtent = false;
    quint32 lba = 0;
    quint64 size = 0;
    QString name; // without ";N" version suffix, "." / ".." returned verbatim
};

quint32 le32(const QByteArray &b, int off)
{
    return static_cast<quint32>(static_cast<quint8>(b[off]))
        | (static_cast<quint32>(static_cast<quint8>(b[off + 1])) << 8)
        | (static_cast<quint32>(static_cast<quint8>(b[off + 2])) << 16)
        | (static_cast<quint32>(static_cast<quint8>(b[off + 3])) << 24);
}

std::optional<DirRecord> parseRecord(const QByteArray &dirData, int *cursor)
{
    if (*cursor + 34 > dirData.size())
        return std::nullopt;
    const int len = static_cast<quint8>(dirData.at(*cursor));
    if (len == 0)
        return std::nullopt; // zero-length padding entries terminate the dir data
    if (*cursor + len > dirData.size())
        return std::nullopt;

    DirRecord r;
    r.lba = le32(dirData, *cursor + 2);
    r.size = le32(dirData, *cursor + 10);
    r.isDir = (static_cast<quint8>(dirData.at(*cursor + 25)) & 0x02) != 0;
    r.multiExtent = (static_cast<quint8>(dirData.at(*cursor + 25)) & 0x80) != 0;
    const int nameLen = static_cast<quint8>(dirData.at(*cursor + 32));
    char ascii[256] = {};
    for (int i = 0; i < nameLen && i < 255; ++i)
        ascii[i] = static_cast<quint8>(dirData.at(*cursor + 33 + i));

    // "." / ".." are stored as 0x00 / 0x01.
    if (nameLen == 1 && (ascii[0] == '\0' || ascii[0] == '\x01'))
    {
        r.name = (ascii[0] == '\0') ? QStringLiteral(".") : QStringLiteral("..");
    }
    else
    {
        QString name = QString::fromLatin1(ascii, nameLen);
        const int semi = name.indexOf(QLatin1Char(';'));
        if (semi >= 0)
            name.truncate(semi);
        r.name = name;
    }

    *cursor += len;
    return r;
}
} // namespace

bool Iso9660::readBlocks(quint32 startLba, quint64 byteLen, const std::function<void(const QByteArray &)> &sink) const
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    const qint64 offset = static_cast<qint64>(startLba) * m_blockSize;
    if (!f.seek(offset))
        return false;

    quint64 remaining = byteLen;
    QByteArray buf;
    buf.reserve(static_cast<int>(std::min<quint64>(256 * 1024, remaining ? remaining : 1)));
    while (remaining > 0)
    {
        const qint64 want = static_cast<qint64>(std::min<quint64>(256 * 1024, remaining));
        QByteArray chunk = f.read(want);
        if (static_cast<quint64>(chunk.size()) != static_cast<quint64>(want))
            return false; // short read: truncated file
        remaining -= static_cast<quint64>(chunk.size());
        sink(chunk);
    }
    return true;
}

bool Iso9660::open(const QString &isoPath)
{
    m_opened = false;
    m_err.clear();
    m_files.clear();
    m_path = isoPath;

    QFile f(isoPath);
    if (!f.open(QIODevice::ReadOnly))
    {
        m_err = QStringLiteral("cannot open %1").arg(isoPath);
        return false;
    }

    // Volume descriptors start at sector 16; a descriptor must fit in one sector.
    QByteArray pvd;
    {
        pvd = f.read(static_cast<qint64>(16) * kSectorSize + kSectorSize);
        if (pvd.size() < static_cast<int>(16) * kSectorSize + kSectorSize)
        {
            m_err = QStringLiteral("file too small to be an ISO image");
            return false;
        }
    }
    pvd = pvd.mid(static_cast<int>(16) * kSectorSize);

    // Volume descriptor: [type(1)][standard-id "CD001"(5)][version(1)].
    if (pvd.size() < 6 || pvd.mid(1, 5) != QByteArrayLiteral("CD001"))
    {
        m_err = QStringLiteral("not an ISO9660 image (missing CD001)");
        return false;
    }
    if (static_cast<quint8>(pvd.at(0)) != 1)
    {
        m_err = QStringLiteral("primary volume descriptor not found");
        return false;
    }

    const quint32 blockSize = static_cast<quint32>(static_cast<quint8>(pvd.at(128)))
        | (static_cast<quint32>(static_cast<quint8>(pvd.at(129))) << 8);
    if (blockSize != kSectorSize)
    {
        m_err = QStringLiteral("unsupported logical block size %1").arg(blockSize);
        return false;
    }

    int cur = 156; // root directory record inside the PVD (both-endian 34-byte)
    auto root = parseRecord(pvd, &cur);
    if (!root || !root->isDir)
    {
        m_err = QStringLiteral("root directory record missing");
        return false;
    }

    m_blockSize = blockSize;
    m_opened = true;

    if (!scanDirectory(root->lba, root->size, QString()))
    {
        m_opened = false;
        m_err = QStringLiteral("failed to read directory tree: %1").arg(m_err);
        return false;
    }
    return true;
}

bool Iso9660::scanDirectory(quint32 lba, quint64 byteLen, const QString &prefix)
{
    QByteArray dirData;
    if (!readBlocks(lba, byteLen, [&](const QByteArray &chunk) { dirData.append(chunk); }))
    {
        m_err = QStringLiteral("short read on directory");
        return false;
    }

    File *lastFile = nullptr;
    int cursor = 0;
    while (cursor < dirData.size())
    {
        const auto rec = parseRecord(dirData, &cursor);
        if (!rec)
            break; // padding terminates the directory listing

        const QString path = prefix.isEmpty() ? rec->name : prefix + QLatin1Char('/') + rec->name;

        if (rec->isDir)
        {
            if (rec->name != QStringLiteral(".") && rec->name != QStringLiteral(".."))
            {
                File d;
                d.path = path;
                d.dir = true;
                m_files.append(d);
                if (!scanDirectory(rec->lba, rec->size, path))
                    return false;
            }
            lastFile = nullptr;
            continue;
        }

        if (rec->multiExtent && lastFile && lastFile->path == path && lastFile->extents.size() >= 1)
        {
            lastFile->extents.append({rec->lba, rec->size});
            continue;
        }

        File f;
        f.path = path;
        f.extents.append({rec->lba, rec->size});
        m_files.append(f);
        lastFile = &m_files.last();
    }
    return true;
}

const Iso9660::File *Iso9660::find(const QString &isoPath) const
{
    QString want = isoPath;
    while (want.startsWith(QLatin1Char('/')))
        want.remove(0, 1);
    while (want.endsWith(QLatin1Char('/')))
        want.chop(1);
    const QString lowerWant = want.toLower();
    for (const File &f : m_files)
    {
        if (!f.dir && f.path.toLower() == lowerWant)
            return &f;
    }
    return nullptr;
}

qint64 Iso9660::readFile(const File &f, const std::function<void(const QByteArray &)> &sink) const
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;

    qint64 total = 0;
    for (const Extent &e : f.extents)
    {
        const qint64 offset = static_cast<qint64>(e.start) * m_blockSize;
        if (!file.seek(offset))
            return -1;
        quint64 left = e.size;
        while (left > 0)
        {
            const qint64 want = static_cast<qint64>(std::min<quint64>(256 * 1024, left));
            QByteArray chunk = file.read(want);
            if (static_cast<quint64>(chunk.size()) != static_cast<quint64>(want))
                return -1;
            left -= static_cast<quint64>(chunk.size());
            total += chunk.size();
            sink(chunk);
        }
    }
    return total;
}

namespace DiscVerify
{

bool verifySlusFromIso(const QString &isoPath)
{
    Iso9660 iso;
    if (!iso.open(isoPath))
        return false;
    const Iso9660::File *slus = iso.find(QStringLiteral("SLUS_216.78"));
    if (!slus)
        return false;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 got = iso.readFile(*slus, [&](const QByteArray &chunk) { hash.addData(chunk); });
    if (got < 0)
        return false;

    return QString::fromLatin1(hash.result().toHex()) == QLatin1String(kExpectedDiscElfSha256);
}

State verifyInstalledData(const QString &dataDir, const QString &bootName,
                          const QString &expectedSha256)
{
    const QString boot = QDir(dataDir).filePath(bootName);
    QFile f(boot);
    if (!f.exists())
        return State::Missing;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!f.open(QIODevice::ReadOnly))
        return State::Corrupt;
    QByteArray buf;
    while (!f.atEnd())
    {
        buf = f.read(256 * 1024);
        if (buf.isEmpty())
            break;
        hash.addData(buf);
    }

    const QString got = QString::fromLatin1(hash.result().toHex());
    return (got.compare(expectedSha256, Qt::CaseInsensitive) == 0) ? State::Valid : State::Corrupt;
}

State verifyInstalledData(const QString &dataDir)
{
    return verifyInstalledData(dataDir, QStringLiteral("SLUS_216.78"),
                               QString::fromLatin1(kExpectedDiscElfSha256));
}

quint64 dataSize(const QString &dataDir)
{
    quint64 total = 0;
    QDirIterator it(dataDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        total += static_cast<quint64>(it.fileInfo().size());
    }
    return total;
}

} // namespace DiscVerify
