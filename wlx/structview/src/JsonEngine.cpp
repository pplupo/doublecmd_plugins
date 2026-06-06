#include "TextFormatEngine.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QHeaderView>
#include <QSet>

/// JSON engine: parses a top-level JSON array of objects into a flat grid.
///
/// Columns = union of all object keys across all array elements.
/// Rows    = one per array element.
/// Nested values are displayed as compact JSON strings in their cells.
///
/// A top-level JSON object is wrapped in a single-element array for display.
class JsonEngine : public TextFormatEngine {
public:
    bool loadInto(QTableWidget *table, const QByteArray &data) override;
    QByteArray serialize(const QTableWidget *table) const override;
    QString formatName() const override { return QStringLiteral("JSON"); }

private:
    // Track original key order for each row (preserves insertion order on save)
    QStringList m_columns;
    bool m_wasObject = false;  // true if the source was a plain object, not an array
};

bool JsonEngine::loadInto(QTableWidget *table, const QByteArray &data)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError)
        return false;

    QJsonArray array;
    if (doc.isArray()) {
        array = doc.array();
        m_wasObject = false;
    } else if (doc.isObject()) {
        array.append(doc.object());
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
                else // object or array — display as compact JSON
                    cellText = QString::fromUtf8(QJsonDocument(v.isObject() ? QJsonDocument(v.toObject()) : QJsonDocument(v.toArray())).toJson(QJsonDocument::Compact));
            }
            table->setItem(r, c, new QTableWidgetItem(cellText));
        }
    }

    table->horizontalHeader()->setStretchLastSection(true);
    return true;
}

QByteArray JsonEngine::serialize(const QTableWidget *table) const
{
    QJsonArray array;
    for (int r = 0; r < table->rowCount(); ++r) {
        QJsonObject obj;
        for (int c = 0; c < table->columnCount(); ++c) {
            QString key = m_columns.value(c, QString::number(c));
            QTableWidgetItem *item = table->item(r, c);
            QString text = item ? item->text() : QString();

            // Try to preserve JSON types
            if (text == QStringLiteral("null")) {
                obj[key] = QJsonValue::Null;
            } else if (text == QStringLiteral("true")) {
                obj[key] = true;
            } else if (text == QStringLiteral("false")) {
                obj[key] = false;
            } else {
                bool ok;
                double num = text.toDouble(&ok);
                if (ok)
                    obj[key] = num;
                else {
                    // Try to parse as nested JSON
                    QJsonDocument nested = QJsonDocument::fromJson(text.toUtf8());
                    if (!nested.isNull())
                        obj[key] = nested.isObject() ? QJsonValue(nested.object()) : QJsonValue(nested.array());
                    else
                        obj[key] = text;
                }
            }
        }
        array.append(obj);
    }

    QJsonDocument doc;
    if (m_wasObject && array.size() == 1)
        doc = QJsonDocument(array[0].toObject());
    else
        doc = QJsonDocument(array);

    return doc.toJson(QJsonDocument::Indented);
}

// Factory helper — called by TextFormatEngine::createForFile()
std::unique_ptr<TextFormatEngine> createJsonEngine()
{
    return std::make_unique<JsonEngine>();
}
