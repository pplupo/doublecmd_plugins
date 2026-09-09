#include "ArchiveSettings.h"

#include <QFileInfo>
#include <QSettings>

// Defined in wlx_entry.cpp for the plugin build. The standalone harnesses do
// not link that translation unit, so they provide their own — see
// tests/scan_smoke.cpp.

ArchiveSettings ArchiveSettings::load(const QString &iniPath)
{
    ArchiveSettings settings;
    if (iniPath.isEmpty() || !QFileInfo::exists(iniPath))
        return settings;

    QSettings ini(iniPath, QSettings::IniFormat);
    ini.beginGroup(QLatin1String(kSection));

    settings.startFlat = ini.value(QStringLiteral("FlatView"),
                                   settings.startFlat).toBool();
    settings.showDetailPanel = ini.value(QStringLiteral("DetailPanel"),
                                         settings.showDetailPanel).toBool();
    settings.showFilterBox = ini.value(QStringLiteral("FilterBox"),
                                       settings.showFilterBox).toBool();

    // A ceiling of zero or less would mean "no listing at all", which is
    // never what someone editing an ini intends; treat it as unset.
    const qint64 ceiling = ini.value(QStringLiteral("MaxEntries"),
                                     settings.maxEntries).toLongLong();
    if (ceiling > 0)
        settings.maxEntries = ceiling;

    settings.nameCodec = ini.value(QStringLiteral("NameCodec")).toString().trimmed();

    // QSettings' ini format treats a comma-separated value as a QStringList
    // on the way in, so "HiddenColumns=CRC-32,Owner" never arrives as one
    // string — but a single-value entry does. Handle both rather than
    // depending on which side of that line the user's ini falls.
    const QVariant hidden = ini.value(QStringLiteral("HiddenColumns"));
    settings.hiddenColumns = hidden.canConvert<QStringList>()
                                 && hidden.metaType().id() == QMetaType::QStringList
                             ? hidden.toStringList()
                             : hidden.toString().split(QLatin1Char(','),
                                                       Qt::SkipEmptyParts);
    for (QString &column : settings.hiddenColumns)
        column = column.trimmed();
    settings.hiddenColumns.removeAll(QString());

    ini.endGroup();
    return settings;
}
