#include "document_engine.h"
#include "mupdf_backend.h"
#include "djvu_backend.h"

#include <algorithm>

static std::string getExtension(const std::string& filepath) {
    auto pos = filepath.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string ext = filepath.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::unique_ptr<DocumentEngine> createDocumentEngine(const std::string& filepath) {
    std::string ext = getExtension(filepath);
    
    if (ext == "djvu" || ext == "djv") {
        return std::make_unique<DjvuBackend>();
    } else if (ext == "pdf" || ext == "epub" || ext == "mobi" || ext == "fb2" || ext == "xps" || ext == "cbz") {
        return std::make_unique<MupdfBackend>();
    }
    
    // Fallback to mupdf if extension is unknown (it can guess by content)
    return std::make_unique<MupdfBackend>();
}
