#pragma once

#include <cairo.h>
#include <librsvg/rsvg.h>
#include <string>

class SvgCore {
public:
    SvgCore();
    ~SvgCore();

    bool load(const char* path);
    
    // Render the SVG into an internal cairo surface matching the viewport size
    void render(int viewport_width, int viewport_height);
    
    // Access the rendered pixel data (ARGB32 premultiplied)
    const unsigned char* get_image_data() const;
    int get_image_stride() const;

    // Export the SVG to PNG at the intrinsic size (or scaled by zoom)
    bool save_png(const char* out_path);
    
    void set_zoom(double zoom);
    void set_pan(double x, double y);
    void zoom_delta(double factor, double center_x, double center_y);

    double get_zoom() const { return m_zoom; }
    double get_pan_x() const { return m_pan_x; }
    double get_pan_y() const { return m_pan_y; }
    int get_intrinsic_width() const { return m_intrinsic_width; }
    int get_intrinsic_height() const { return m_intrinsic_height; }

private:
    void draw_checkerboard(cairo_t* cr, int width, int height);
    void free_surface();

    RsvgHandle* m_handle;
    cairo_surface_t* m_surface;
    
    double m_zoom;
    double m_pan_x;
    double m_pan_y;
    int m_intrinsic_width;
    int m_intrinsic_height;
};
