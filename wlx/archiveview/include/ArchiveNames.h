#pragma once

#include <QByteArray>
#include <QString>
#include <QStringDecoder>

/// Member-name handling shared by the libarchive walk and the ZIP central
/// directory parser.
///
/// This is not here for the sake of deduplication — it is here because the
/// two readers must agree *exactly*. The parser's per-entry packed size and
/// CRC are joined onto the scanner's entries by path, so any divergence in
/// how a name is decoded or normalised silently drops the extra columns
/// instead of failing loudly.

namespace ArchiveNames {

/// Codec applied to names that are neither valid UTF-8 nor decodable in the
/// current locale, from the NameCodec ini setting. Set once before a scan;
/// read from the scanner thread afterwards.
///
/// A single process-wide value rather than a parameter threaded through every
/// call: both readers must agree on it for the central-directory join to
/// line up, and a viewer only ever has one archive open at a time.
inline QString &fallbackCodec()
{
    static QString codec;
    return codec;
}

/// Decode a member name without assuming UTF-8.
///
/// Passing raw bytes into a QString is an implicit fromUtf8() and mangles
/// every archive whose names are in a legacy codepage. libarchive's *_utf8
/// accessors convert properly but return NULL when the name cannot be
/// represented, so both paths are needed — and either can be NULL.
///
/// Deliberately no enca detection: it is unreliable on a short filename, and
/// 100k detections would cost more than the entire archive walk. Latin-1 is
/// the last resort because it round-trips losslessly — the bytes survive even
/// when the glyphs are wrong.
inline QString decode(const QByteArray &raw, bool declaredUtf8 = false)
{
    if (raw.isEmpty())
        return QString();

    if (declaredUtf8)
        return QString::fromUtf8(raw);

    auto utf8 = QStringDecoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString decoded = utf8(raw);
    if (!utf8.hasError())
        return decoded;

    auto local = QStringDecoder(QStringDecoder::System, QStringDecoder::Flag::Stateless);
    decoded = local(raw);
    if (!local.hasError())
        return decoded;

    // An explicitly configured legacy codepage. This is the only way to get
    // these names right: the archive does not record its encoding, so no
    // amount of detection can do better than a user who knows where the file
    // came from.
    const QString &configured = fallbackCodec();
    if (!configured.isEmpty()) {
        auto codec = QStringDecoder(configured.toUtf8().constData(),
                                    QStringDecoder::Flag::Stateless);
        if (codec.isValid()) {
            decoded = codec(raw);
            if (!codec.hasError())
                return decoded;
        }
    }

    return QString::fromLatin1(raw);
}

/// Normalise a member path for tree building: strip the "./" prefix tar
/// archives carry, collapse runs of separators so "a//b.txt" does not produce
/// empty-named tree nodes, and drop any trailing slash so "a/b/" and "a/b"
/// land on the same node.
///
/// Note what this deliberately does NOT do: it does not resolve "..", and it
/// does not strip a leading "/". A member named "../../../../etc/passwd" is
/// shown exactly as stored. Sanitising it here would hide the single most
/// useful thing this viewer can tell you about a hostile archive.
inline QString normalize(QString path)
{
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);

    while (path.contains(QLatin1String("//")))
        path.replace(QLatin1String("//"), QLatin1String("/"));

    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);

    return path;
}

} // namespace ArchiveNames
