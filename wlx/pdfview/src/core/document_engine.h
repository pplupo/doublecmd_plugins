#ifndef DOCUMENT_ENGINE_H
#define DOCUMENT_ENGINE_H

#include <string>
#include <vector>
#include <memory>

struct Rect {
    float x0, y0, x1, y1;
};

struct TextBlock {
    std::string text;
    Rect bbox;
    // Index of the text line/row this character belongs to (0, 1, 2, ...,
    // in reading order), so callers can do row-aware selection --
    // "from here to end of this row, all of the next N rows, up to there
    // on the last row" -- instead of a plain bounding-box rectangle,
    // which is wrong for indented/justified/multi-line text. -1 if the
    // backend doesn't report line grouping (selection then falls back to
    // pure bbox-rectangle behavior).
    int row = -1;
};

class DocumentEngine {
public:
    virtual ~DocumentEngine() = default;

    virtual bool load(const std::string& filepath) = 0;
    virtual int getPageCount() const = 0;

    // Native page size in points (i.e. at zoom = 1.0), without rendering
    // it -- used to lay out the whole document for continuous scrolling
    // without rasterizing every page up front.
    virtual bool getPageSize(int pageNumber, float& outWidth, float& outHeight) = 0;

    // Renders the page into an RGBA32 buffer
    virtual std::vector<unsigned char> renderPage(int pageNumber, float zoom, int& outWidth, int& outHeight) = 0;

    // Returns text blocks for the given page (for selection)
    virtual std::vector<TextBlock> getText(int pageNumber) = 0;
};

// Factory to instantiate the correct backend
std::unique_ptr<DocumentEngine> createDocumentEngine(const std::string& filepath);

#endif // DOCUMENT_ENGINE_H
