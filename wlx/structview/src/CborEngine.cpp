#include "TextFormatEngine.h"

#include <QCborValue>
#include <QCborMap>
#include <QCborArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHeaderView>

/// CBOR engine: converts CBOR ↔ JSON and delegates grid layout to the same
/// flattening logic used by JsonEngine.
///
/// Strategy:
///   Load:  QCborValue::fromCbor() → toJsonValue() → flatten as JSON
///   Save:  grid → JSON → QCborValue::fromJsonValue() → toCbor()
///
/// This reuses the JSON array-of-objects grid pattern without code duplication.
class CborEngine : public TextFormatEngine {
public:
    bool loadInto(QTableWidget *table, const QByteArray &data) override;
    QByteArray serialize(const QTableWidget *table) const override;
    QString formatName() const override { return QStringLiteral("CBOR"); }

private:
    QStringList m_columns;
    bool m_wasObject = false;
};

bool CborEngine::loadInto(QTableWidget *table, const QByteArray &data)
{
    QCborParserError err;
    QCborValue cbor = QCborValue::fromCbor(data, &err);
    if (err.error != QCborError::NoError)
        return false;

    // Convert CBOR to JSON for flattening
    QJsonValue jsonVal = cbor.toJsonValue();

    QJsonArray array;
    if (jsonVal.isArray()) {
        array = jsonVal.toArray();
        m_wasObject = false;
    } else if (jsonVal.isObject()) {
        array.append(jsonVal.toObject());
        m_wasObject = true;
    } else {
        return false;
    }

    if (array.isEmpty())
        return false;

    // Collect all unique keys in order of first appearance
    QSet<QString> seen;
    m_columns.clear();
    for (const QJsonValue &val : array) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!seen.contains(it.key())) {
                seen.insert(it.key());
                m_columns.append(it.key());
            }
        }
    }

    table->setColumnCount(m_columns.size());
    table->setHorizontalHeaderLabels(m_columns);
    table->setRowCount(array.size());

    for (int r = 0; r < array.size(); ++r) {
        QJsonObject obj = array[r].toObject();
        for (int c = 0; c < m_columns.size(); ++c) {
            QString key = m_columns[c];
            QString cellText;
            if (obj.contains(key)) {
                QJsonValue v = obj[key];
                if (v.isString())
                    cellText = v.toString();
                else if (v.isDouble())
                    cellText = QString::number(v.toDouble(), 'g', 15);
                else if (v.isBool())
                    cellText = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
                else if (v.isNull())
                    cellText = QStringLiteral("null");
                else
                    cellText = QString::fromUtf8(
                        QJsonDocument(v.isObject() ? QJsonDocument(v.toObject())
                                                   : QJsonDocument(v.toArray()))
                            .toJson(QJsonDocument::Compact));
            }
            table->setItem(r, c, new QTableWidgetItem(cellText));
        }
    }

    table->horizontalHeader()->setStretchLastSection(true);
    return true;
}

QByteArray CborEngine::serialize(const QTableWidget *table) const
{
    QJsonArray array;
    for (int r = 0; r < table->rowCount(); ++r) {
        QJsonObject obj;
        for (int c = 0; c < table->columnCount(); ++c) {
            QString key = m_columns.value(c, QString::number(c));
            QTableWidgetItem *item = table->item(r, c);
            QString text = item ? item->text() : QString();

            if (text == QStringLiteral("null"))
                obj[key] = QJsonValue::Null;
            else if (text == QStringLiteral("true"))
                obj[key] = true;
            else if (text == QStringLiteral("false"))
                obj[key] = false;
            else {
                bool ok;
                double num = text.toDouble(&ok);
                if (ok)
                    obj[key] = num;
                else {
                    QJsonDocument nested = QJsonDocument::fromJson(text.toUtf8());
                    if (!nested.isNull())
                        obj[key] = nested.isObject() ? QJsonValue(nested.object())
                                                     : QJsonValue(nested.array());
                    else
                        obj[key] = text;
                }
            }
        }
        array.append(obj);
    }

    QJsonValue jsonVal;
    if (m_wasObject && array.size() == 1)
        jsonVal = array[0];
    else
        jsonVal = array;

    QCborValue cbor = QCborValue::fromJsonValue(jsonVal);
    return cbor.toCbor();
}

// Factory helper — called by TextFormatEngine::createForFile()
std::unique_ptr<TextFormatEngine> createCborEngine()
{
    return std::make_unique<CborEngine>();
}
