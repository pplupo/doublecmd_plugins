#ifndef MUPDF_BACKEND_H
#define MUPDF_BACKEND_H

#include "document_engine.h"
#include <mupdf/fitz.h>

class MupdfBackend : public DocumentEngine {
public:
    MupdfBackend();
    ~MupdfBackend() override;

    bool load(const std::string& filepath) override;
    int getPageCount() const override;
    bool getPageSize(int pageNumber, float& outWidth, float& outHeight) override;
    std::vector<unsigned char> renderPage(int pageNumber, float zoom, int& outWidth, int& outHeight) override;
    std::vector<TextBlock> getText(int pageNumber) override;

private:
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
};

#endif // MUPDF_BACKEND_H
