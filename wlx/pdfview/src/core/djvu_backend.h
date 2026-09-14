#ifndef DJVU_BACKEND_H
#define DJVU_BACKEND_H

#include "document_engine.h"
#include <libdjvu/ddjvuapi.h>

class DjvuBackend : public DocumentEngine {
public:
    DjvuBackend();
    ~DjvuBackend() override;

    bool load(const std::string& filepath) override;
    int getPageCount() const override;
    bool getPageSize(int pageNumber, float& outWidth, float& outHeight) override;
    std::vector<unsigned char> renderPage(int pageNumber, float zoom, int& outWidth, int& outHeight) override;
    std::vector<TextBlock> getText(int pageNumber) override;

private:
    ddjvu_context_t* ctx = nullptr;
    ddjvu_document_t* doc = nullptr;
};

#endif // DJVU_BACKEND_H
