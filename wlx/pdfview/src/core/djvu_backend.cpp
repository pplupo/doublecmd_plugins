#include "djvu_backend.h"
#include <iostream>
#include <thread>
#include <chrono>

DjvuBackend::DjvuBackend() {
    ctx = ddjvu_context_create("pdfview");
}

DjvuBackend::~DjvuBackend() {
    if (doc) ddjvu_document_release(doc);
    if (ctx) ddjvu_context_release(ctx);
}

bool DjvuBackend::load(const std::string& filepath) {
    if (!ctx) return false;
    doc = ddjvu_document_create_by_filename(ctx, filepath.c_str(), TRUE);
    if (!doc) return false;
    
    // Wait for document to be parsed
    ddjvu_message_wait(ctx);
    while (ddjvu_message_peek(ctx)) {
        ddjvu_message_pop(ctx);
    }
    return true;
}

int DjvuBackend::getPageCount() const {
    if (!doc) return 0;
    return ddjvu_document_get_pagenum(doc);
}

bool DjvuBackend::getPageSize(int pageNumber, float& outWidth, float& outHeight) {
    if (!doc) return false;
    ddjvu_page_t* page = ddjvu_page_create_by_pageno(doc, pageNumber);
    if (!page) return false;

    while (!ddjvu_page_decoding_done(page)) {
        ddjvu_message_wait(ctx);
        while (ddjvu_message_peek(ctx)) ddjvu_message_pop(ctx);
    }

    outWidth = static_cast<float>(ddjvu_page_get_width(page));
    outHeight = static_cast<float>(ddjvu_page_get_height(page));
    ddjvu_page_release(page);
    return true;
}

std::vector<unsigned char> DjvuBackend::renderPage(int pageNumber, float zoom, int& outWidth, int& outHeight) {
    std::vector<unsigned char> buffer;
    outWidth = 0;
    outHeight = 0;
    if (!doc) return buffer;

    ddjvu_page_t* page = ddjvu_page_create_by_pageno(doc, pageNumber);
    if (!page) return buffer;
    
    // Wait for page to decode
    while (!ddjvu_page_decoding_done(page)) {
        ddjvu_message_wait(ctx);
        while (ddjvu_message_peek(ctx)) ddjvu_message_pop(ctx);
    }
    
    int w = ddjvu_page_get_width(page);
    int h = ddjvu_page_get_height(page);
    
    outWidth = static_cast<int>(w * zoom);
    outHeight = static_cast<int>(h * zoom);
    
    ddjvu_rect_t rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = outWidth;
    rect.h = outHeight;
    
    // libdjvu has no BGRA/RGBA format constant; RGBMASK32 with byte-order
    // R,G,B masks lands the color bytes at the right offsets directly, but
    // leaves the 4th (alpha) byte of each pixel untouched, so it's filled
    // in below.
    unsigned int masks[3] = {0x000000ff, 0x0000ff00, 0x00ff0000};
    ddjvu_format_t* fmt = ddjvu_format_create(DDJVU_FORMAT_RGBMASK32, 3, masks);
    ddjvu_format_set_row_order(fmt, TRUE);

    int size = outWidth * outHeight * 4;
    buffer.resize(size);

    ddjvu_page_render(page, DDJVU_RENDER_COLOR, &rect, &rect, fmt, outWidth * 4, (char*)buffer.data());

    for (int i = 0; i < outWidth * outHeight; ++i) {
        buffer[i * 4 + 3] = 0xFF;
    }

    ddjvu_format_release(fmt);
    ddjvu_page_release(page);
    
    return buffer;
}

std::vector<TextBlock> DjvuBackend::getText(int pageNumber) {
    std::vector<TextBlock> blocks;
    // Basic stub, real implementation uses miniexp for HIDDENTEXT
    return blocks;
}
