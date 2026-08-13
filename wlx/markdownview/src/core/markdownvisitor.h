#pragma once

#include "html.h"

class MarkdownVisitor : public MD::details::HtmlVisitor {
public:
    explicit MarkdownVisitor(bool darkMode = false);
    ~MarkdownVisitor() override;

    void setDarkMode(bool darkMode) { m_darkMode = darkMode; }
    bool isDarkMode() const { return m_darkMode; }

protected:
    void onMath(MD::Math *m) override;
    void onCode(MD::Code *c) override;
    void onImage(MD::Image *i) override;

private:
    QByteArray runMermaidWeb(const QString& code);
    QByteArray runPlantUmlWeb(const QString& puml);
    QByteArray fixMermaidSvgText(const QByteArray& svgData);
    QString fixPlantUmlSvgDark(const QString& svgStr);
    QByteArray svgToHighDpiPng(const QByteArray& svgData, float scale, int& logicalWidth, int& logicalHeight);

    bool m_darkMode = false;
};
