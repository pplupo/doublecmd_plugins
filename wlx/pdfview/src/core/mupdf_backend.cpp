#include "mupdf_backend.h"
#include <iostream>

MupdfBackend::MupdfBackend() {
    ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!ctx) {
        std::cerr << "Cannot create MuPDF context" << std::endl;
    }
    fz_register_document_handlers(ctx);
}

MupdfBackend::~MupdfBackend() {
    if (doc) fz_drop_document(ctx, doc);
    if (ctx) fz_drop_context(ctx);
}

bool MupdfBackend::load(const std::string& filepath) {
    if (!ctx) return false;
    fz_try(ctx) {
        doc = fz_open_document(ctx, filepath.c_str());
    } fz_catch(ctx) {
        std::cerr << "Failed to open document: " << filepath << std::endl;
        return false;
    }
    return doc != nullptr;
}

int MupdfBackend::getPageCount() const {
    if (!ctx || !doc) return 0;
    return fz_count_pages(ctx, doc);
}

bool MupdfBackend::getPageSize(int pageNumber, float& outWidth, float& outHeight) {
    if (!ctx || !doc) return false;
    bool ok = false;
    fz_try(ctx) {
        fz_page* page = fz_load_page(ctx, doc, pageNumber);
        fz_rect bounds = fz_bound_page(ctx, page);
        outWidth = bounds.x1 - bounds.x0;
        outHeight = bounds.y1 - bounds.y0;
        fz_drop_page(ctx, page);
        ok = true;
    } fz_catch(ctx) {
        std::cerr << "Error getting page size" << std::endl;
    }
    return ok;
}

std::vector<unsigned char> MupdfBackend::renderPage(int pageNumber, float zoom, int& outWidth, int& outHeight) {
    std::vector<unsigned char> buffer;
    outWidth = 0;
    outHeight = 0;
    
    if (!ctx || !doc) return buffer;
    
    fz_try(ctx) {
        fz_page* page = fz_load_page(ctx, doc, pageNumber);
        
        fz_matrix transform = fz_scale(zoom, zoom);
        fz_rect bounds = fz_bound_page(ctx, page);
        bounds = fz_transform_rect(bounds, transform);
        
        fz_irect bbox = fz_round_rect(bounds);
        
        fz_pixmap* pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pix, 0xff);

        fz_device* dev = fz_new_draw_device(ctx, transform, pix);
        fz_run_page(ctx, page, dev, fz_identity, nullptr);
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);

        outWidth = pix->w;
        outHeight = pix->h;

        int size = outWidth * outHeight * 4;
        buffer.resize(size);

        // Convert RGB to RGBA, row by row -- pix->stride may include
        // padding beyond outWidth * 3, so it can't be treated as a flat
        // outWidth * outHeight * 3 buffer.
        for (int y = 0; y < outHeight; ++y) {
            const unsigned char* srcRow = pix->samples + y * pix->stride;
            unsigned char* dstRow = buffer.data() + y * outWidth * 4;
            for (int x = 0; x < outWidth; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0]; // R
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2]; // B
                dstRow[x * 4 + 3] = 255;               // A
            }
        }
        
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
    } fz_catch(ctx) {
        std::cerr << "Error rendering page" << std::endl;
    }
    
    return buffer;
}

std::vector<TextBlock> MupdfBackend::getText(int pageNumber) {
    std::vector<TextBlock> blocks;
    if (!ctx || !doc) return blocks;
    
    fz_try(ctx) {
        fz_page* page = fz_load_page(ctx, doc, pageNumber);
        fz_stext_page* text_page = fz_new_stext_page_from_page(ctx, page, nullptr);
        
        int rowIndex = -1;
        for (fz_stext_block* block = text_page->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                ++rowIndex; // each fz_stext_line is a real text row/line
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                    TextBlock tb;
                    // Note: UTF-8 conversion simplified for demonstration
                    char utf8[10];
                    int len = fz_runetochar(utf8, ch->c);
                    utf8[len] = '\0';
                    tb.text = utf8;
                    tb.bbox.x0 = ch->quad.ul.x;
                    tb.bbox.y0 = ch->quad.ul.y;
                    tb.bbox.x1 = ch->quad.lr.x;
                    tb.bbox.y1 = ch->quad.lr.y;
                    tb.row = rowIndex;
                    blocks.push_back(tb);
                }
            }
        }
        
        fz_drop_stext_page(ctx, text_page);
        fz_drop_page(ctx, page);
    } fz_catch(ctx) {
        std::cerr << "Error extracting text" << std::endl;
    }
    
    return blocks;
}
