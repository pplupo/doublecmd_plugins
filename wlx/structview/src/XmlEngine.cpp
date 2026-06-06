#include "TextFormatEngine.h"

#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QHeaderView>
#include <QSet>

/// XML engine: flattens repeating child elements into a grid.
///
/// Strategy:
///  1. Find the root element.
///  2. Find the most common repeating child tag name (the "row" element).
///  3. Columns = union of all child element tag names + attribute names
///     of those repeating elements.
///  4. Each repeating element → one row.
///
/// Non-repeating elements or text-only roots fall back to a 2-column
/// Name | Value layout showing all children.
class XmlEngine : public TextFormatEngine {
public:
    bool loadInto(QTableWidget *table, const QByteArray &data) override;
    QByteArray serialize(const QTableWidget *table) const override;
    QString formatName() const override { return QStringLiteral("XML"); }

private:
    QStringList m_columns;
    QString m_rootTag;
    QString m_rowTag;
    QDomDocument m_originalDoc;
    bool m_isFlatLayout = false;
};

bool XmlEngine::loadInto(QTableWidget *table, const QByteArray &data)
{
    QString errorMsg;
    int errorLine, errorCol;
    if (!m_originalDoc.setContent(data, &errorMsg, &errorLine, &errorCol))
        return false;

    QDomElement root = m_originalDoc.documentElement();
    if (root.isNull())
        return false;
    m_rootTag = root.tagName();

    // Count child element tag frequencies to find the "row" tag
    QMap<QString, int> tagCounts;
    QDomNodeList children = root.childNodes();
    for (int i = 0; i < children.count(); ++i) {
        QDomNode child = children.at(i);
        if (child.isElement())
            tagCounts[child.toElement().tagName()]++;
    }

    // Find the most frequent child tag
    int maxCount = 0;
    for (auto it = tagCounts.begin(); it != tagCounts.end(); ++it) {
        if (it.value() > maxCount) {
            maxCount = it.value();
            m_rowTag = it.key();
        }
    }

    // If no repeating pattern (or only 1 child), fall back to flat Name|Value layout
    if (maxCount <= 1) {
        m_isFlatLayout = true;
        m_columns = { QStringLiteral("Name"), QStringLiteral("Value") };
        table->setColumnCount(2);
        table->setHorizontalHeaderLabels(m_columns);

        // Collect all child elements
        QList<QPair<QString, QString>> rows;
        for (int i = 0; i < children.count(); ++i) {
            QDomNode child = children.at(i);
            if (child.isElement()) {
                QDomElement el = child.toElement();
                rows.append({ el.tagName(), el.text() });
            }
        }

        table->setRowCount(rows.size());
        for (int r = 0; r < rows.size(); ++r) {
            table->setItem(r, 0, new QTableWidgetItem(rows[r].first));
            table->setItem(r, 1, new QTableWidgetItem(rows[r].second));
        }
        table->horizontalHeader()->setStretchLastSection(true);
        return true;
    }

    m_isFlatLayout = false;

    // Collect columns: attributes + child element tag names of the row elements
    QSet<QString> seen;
    m_columns.clear();

    for (int i = 0; i < children.count(); ++i) {
        QDomNode child = children.at(i);
        if (!child.isElement() || child.toElement().tagName() != m_rowTag)
            continue;
        QDomElement rowEl = child.toElement();

        // Attributes first (prefixed with @)
        QDomNamedNodeMap attrs = rowEl.attributes();
        for (int a = 0; a < attrs.count(); ++a) {
            QString attrName = QStringLiteral("@") + attrs.item(a).toAttr().name();
            if (!seen.contains(attrName)) {
                seen.insert(attrName);
                m_columns.append(attrName);
            }
        }

        // Child elements
        QDomNodeList rowChildren = rowEl.childNodes();
        for (int j = 0; j < rowChildren.count(); ++j) {
            QDomNode rc = rowChildren.at(j);
            if (rc.isElement()) {
                QString tag = rc.toElement().tagName();
                if (!seen.contains(tag)) {
                    seen.insert(tag);
                    m_columns.append(tag);
                }
            }
        }
    }

    table->setColumnCount(m_columns.size());
    table->setHorizontalHeaderLabels(m_columns);

    // Populate rows
    int rowIndex = 0;
    int totalRows = 0;
    for (int i = 0; i < children.count(); ++i) {
        if (children.at(i).isElement() && children.at(i).toElement().tagName() == m_rowTag)
            totalRows++;
    }
    table->setRowCount(totalRows);

    for (int i = 0; i < children.count(); ++i) {
        QDomNode child = children.at(i);
        if (!child.isElement() || child.toElement().tagName() != m_rowTag)
            continue;

        QDomElement rowEl = child.toElement();

        for (int c = 0; c < m_columns.size(); ++c) {
            QString colName = m_columns[c];
            QString cellText;

            if (colName.startsWith('@')) {
                // Attribute
                cellText = rowEl.attribute(colName.mid(1));
            } else {
                // Child element text
                QDomElement sub = rowEl.firstChildElement(colName);
                if (!sub.isNull())
                    cellText = sub.text();
            }
            table->setItem(rowIndex, c, new QTableWidgetItem(cellText));
        }
        rowIndex++;
    }

    table->horizontalHeader()->setStretchLastSection(true);
    return true;
}

QByteArray XmlEngine::serialize(const QTableWidget *table) const
{
    QDomDocument doc;
    QDomProcessingInstruction pi = doc.createProcessingInstruction(
        QStringLiteral("xml"), QStringLiteral("version=\"1.0\" encoding=\"UTF-8\""));
    doc.appendChild(pi);

    QDomElement root = doc.createElement(m_rootTag);
    doc.appendChild(root);

    if (m_isFlatLayout) {
        for (int r = 0; r < table->rowCount(); ++r) {
            QString name = table->item(r, 0) ? table->item(r, 0)->text() : QString();
            QString value = table->item(r, 1) ? table->item(r, 1)->text() : QString();
            if (name.isEmpty()) continue;
            QDomElement el = doc.createElement(name);
            el.appendChild(doc.createTextNode(value));
            root.appendChild(el);
        }
    } else {
        for (int r = 0; r < table->rowCount(); ++r) {
            QDomElement rowEl = doc.createElement(m_rowTag);
            for (int c = 0; c < table->columnCount(); ++c) {
                QString colName = m_columns.value(c);
                QTableWidgetItem *item = table->item(r, c);
                QString text = item ? item->text() : QString();

                if (colName.startsWith('@')) {
                    if (!text.isEmpty())
                        rowEl.setAttribute(colName.mid(1), text);
                } else {
                    QDomElement child = doc.createElement(colName);
                    child.appendChild(doc.createTextNode(text));
                    rowEl.appendChild(child);
                }
            }
            root.appendChild(rowEl);
        }
    }

    return doc.toByteArray(2);
}

// Factory helper — called by TextFormatEngine::createForFile()
std::unique_ptr<TextFormatEngine> createXmlEngine()
{
    return std::make_unique<XmlEngine>();
}
