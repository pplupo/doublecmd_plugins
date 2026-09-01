#include "svg_core.h"
#include <algorithm>
#include <iostream>

SvgCore::SvgCore()
    : m_handle(nullptr), m_surface(nullptr),
      m_zoom(1.0), m_pan_x(0.0), m_pan_y(0.0),
      m_intrinsic_width(800), m_intrinsic_height(600)
{
}

SvgCore::~SvgCore()
{
    free_surface();
    if (m_handle) {
        g_object_unref(m_handle);
    }
}

void SvgCore::free_surface()
{
    if (m_surface) {
        cairo_surface_destroy(m_surface);
        m_surface = nullptr;
    }
}

bool SvgCore::load(const char* path)
{
    if (m_handle) {
        g_object_unref(m_handle);
        m_handle = nullptr;
    }

    GError* error = nullptr;
    m_handle = rsvg_handle_new_from_file(path, &error);

    if (!m_handle) {
        if (error) {
            std::cerr << "Failed to load SVG: " << error->message << std::endl;
            g_error_free(error);
        }
        return false;
    }

    gdouble out_width, out_height;
    if (rsvg_handle_get_intrinsic_size_in_pixels(m_handle, &out_width, &out_height)) {
        m_intrinsic_width = std::max(1, (int)out_width);
        m_intrinsic_height = std::max(1, (int)out_height);
    } else {
        gboolean has_width, has_height, has_viewbox;
        RsvgLength iwidth, iheight;
        RsvgRectangle viewBox;
        rsvg_handle_get_intrinsic_dimensions(m_handle, &has_width, &iwidth, &has_height, &iheight, &has_viewbox, &viewBox);
        
        if (has_viewbox) {
            m_intrinsic_width = std::max(1, (int)viewBox.width);
            m_intrinsic_height = std::max(1, (int)viewBox.height);
        } else {
            m_intrinsic_width = 800;
            m_intrinsic_height = 600;
        }
    }

    m_zoom = 1.0;
    m_pan_x = 0.0;
    m_pan_y = 0.0;

    return true;
}

void SvgCore::draw_checkerboard(cairo_t* cr, int width, int height)
{
    int square_size = 16;
    for (int y = 0; y < height; y += square_size) {
        for (int x = 0; x < width; x += square_size) {
            if (((x / square_size) + (y / square_size)) % 2 == 0) {
                cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
            } else {
                cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
            }
            cairo_rectangle(cr, x, y, square_size, square_size);
            cairo_fill(cr);
        }
    }
}

void SvgCore::render(int viewport_width, int viewport_height)
{
    if (viewport_width <= 0 || viewport_height <= 0) return;

    free_surface();
    m_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, viewport_width, viewport_height);
    
    if (!m_surface || cairo_surface_status(m_surface) != CAIRO_STATUS_SUCCESS) {
        return;
    }

    cairo_t* cr = cairo_create(m_surface);

    // Draw background
    draw_checkerboard(cr, viewport_width, viewport_height);

    if (m_handle) {
        cairo_save(cr);
        
        // Apply panning and zooming
        cairo_translate(cr, -m_pan_x, -m_pan_y);
        cairo_scale(cr, m_zoom, m_zoom);

        RsvgRectangle viewport;
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = m_intrinsic_width;
        viewport.height = m_intrinsic_height;

        GError* error = nullptr;
        if (!rsvg_handle_render_document(m_handle, cr, &viewport, &error)) {
            if (error) {
                std::cerr << "Render error: " << error->message << std::endl;
                g_error_free(error);
            }
        }
        
        cairo_restore(cr);
    }

    cairo_destroy(cr);
}

const unsigned char* SvgCore::get_image_data() const
{
    if (!m_surface) return nullptr;
    cairo_surface_flush(m_surface);
    return cairo_image_surface_get_data(m_surface);
}

int SvgCore::get_image_stride() const
{
    if (!m_surface) return 0;
    return cairo_image_surface_get_stride(m_surface);
}

bool SvgCore::save_png(const char* out_path)
{
    if (!m_handle) return false;

    int width = std::max(1, (int)(m_intrinsic_width * m_zoom));
    int height = std::max(1, (int)(m_intrinsic_height * m_zoom));

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(surface);

    // Ensure transparent background
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_scale(cr, m_zoom, m_zoom);

    RsvgRectangle viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = m_intrinsic_width;
    viewport.height = m_intrinsic_height;

    rsvg_handle_render_document(m_handle, cr, &viewport, nullptr);

    cairo_surface_write_to_png(surface, out_path);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return true;
}

void SvgCore::set_zoom(double zoom)
{
    m_zoom = std::max(0.1, std::min(zoom, 50.0));
}

void SvgCore::set_pan(double x, double y)
{
    m_pan_x = std::max(0.0, x);
    m_pan_y = std::max(0.0, y);
}

void SvgCore::zoom_delta(double factor, double center_x, double center_y)
{
    double old_zoom = m_zoom;
    set_zoom(m_zoom * factor);
    
    // Adjust pan to zoom towards the center point
    double doc_x = (m_pan_x + center_x) / old_zoom;
    double doc_y = (m_pan_y + center_y) / old_zoom;

    set_pan(doc_x * m_zoom - center_x, doc_y * m_zoom - center_y);
}
