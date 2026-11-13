#include "LinuxPlatform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// ============================================================================
// Minimal Cairo / Pango / GdkPixbuf / Fontconfig C ABI declarations (via dlopen)
// ============================================================================

struct cairo_t;
struct cairo_surface_t;
struct cairo_pattern_t;
struct cairo_font_options_t;
struct PangoLayout;
struct PangoFontDescription;

struct cairo_matrix_t {
    double xx, yx;
    double xy, yy;
    double x0, y0;
};

struct PangoRectangle {
    int x;
    int y;
    int width;
    int height;
};

enum cairo_status_t {
    CAIRO_STATUS_SUCCESS = 0
};

enum cairo_format_t {
    CAIRO_FORMAT_INVALID = -1,
    CAIRO_FORMAT_ARGB32 = 0,
    CAIRO_FORMAT_RGB24 = 1,
    CAIRO_FORMAT_A8 = 2
};

enum cairo_operator_t {
    CAIRO_OPERATOR_CLEAR = 0,
    CAIRO_OPERATOR_SOURCE = 1,
    CAIRO_OPERATOR_OVER = 2
};

enum cairo_fill_rule_t {
    CAIRO_FILL_RULE_WINDING = 0,
    CAIRO_FILL_RULE_EVEN_ODD = 1
};

enum cairo_line_cap_t {
    CAIRO_LINE_CAP_BUTT = 0,
    CAIRO_LINE_CAP_ROUND = 1,
    CAIRO_LINE_CAP_SQUARE = 2
};

enum cairo_line_join_t {
    CAIRO_LINE_JOIN_MITER = 0,
    CAIRO_LINE_JOIN_ROUND = 1,
    CAIRO_LINE_JOIN_BEVEL = 2
};

enum cairo_antialias_t {
    CAIRO_ANTIALIAS_DEFAULT = 0,
    CAIRO_ANTIALIAS_NONE = 1,
    CAIRO_ANTIALIAS_GRAY = 2,
    CAIRO_ANTIALIAS_SUBPIXEL = 3
};

enum cairo_subpixel_order_t {
    CAIRO_SUBPIXEL_ORDER_DEFAULT = 0,
    CAIRO_SUBPIXEL_ORDER_RGB = 1
};

enum cairo_hint_style_t {
    CAIRO_HINT_STYLE_DEFAULT = 0,
    CAIRO_HINT_STYLE_NONE = 1,
    CAIRO_HINT_STYLE_SLIGHT = 2,
    CAIRO_HINT_STYLE_MEDIUM = 3,
    CAIRO_HINT_STYLE_FULL = 4
};

enum cairo_hint_metrics_t {
    CAIRO_HINT_METRICS_DEFAULT = 0,
    CAIRO_HINT_METRICS_OFF = 1,
    CAIRO_HINT_METRICS_ON = 2
};

enum cairo_extend_t {
    CAIRO_EXTEND_NONE = 0,
    CAIRO_EXTEND_REPEAT = 1,
    CAIRO_EXTEND_REFLECT = 2,
    CAIRO_EXTEND_PAD = 3
};

enum cairo_filter_t {
    CAIRO_FILTER_FAST = 0,
    CAIRO_FILTER_GOOD = 1,
    CAIRO_FILTER_BEST = 2,
    CAIRO_FILTER_NEAREST = 3,
    CAIRO_FILTER_BILINEAR = 4
};

enum PangoStyle {
    PANGO_STYLE_NORMAL = 0,
    PANGO_STYLE_OBLIQUE = 1,
    PANGO_STYLE_ITALIC = 2
};

enum PangoAlignment {
    PANGO_ALIGN_LEFT = 0,
    PANGO_ALIGN_CENTER = 1,
    PANGO_ALIGN_RIGHT = 2
};

enum PangoWrapMode {
    PANGO_WRAP_WORD = 0,
    PANGO_WRAP_CHAR = 1,
    PANGO_WRAP_WORD_CHAR = 2
};

enum PangoEllipsizeMode {
    PANGO_ELLIPSIZE_NONE = 0,
    PANGO_ELLIPSIZE_START = 1,
    PANGO_ELLIPSIZE_MIDDLE = 2,
    PANGO_ELLIPSIZE_END = 3
};

static constexpr int kPangoScale = 1024;
static constexpr double kPi = 3.14159265358979323846;

using cairo_read_func_t = cairo_status_t (*)(void* closure, unsigned char* data, unsigned int length);
using cairo_write_func_t = cairo_status_t (*)(void* closure, const unsigned char* data, unsigned int length);

struct CairoPangoApi {
    void* libcairo = nullptr;
    void* libpangocairo = nullptr;
    void* libpango = nullptr;
    void* libpangoft2 = nullptr;
    void* libgobject = nullptr;
    bool loaded = false;

    cairo_surface_t* (*cairo_image_surface_create)(cairo_format_t, int, int) = nullptr;
    cairo_surface_t* (*cairo_image_surface_create_for_data)(unsigned char*, cairo_format_t, int, int, int) = nullptr;
    cairo_surface_t* (*cairo_image_surface_create_from_png_stream)(cairo_read_func_t, void*) = nullptr;
    cairo_status_t (*cairo_surface_write_to_png_stream)(cairo_surface_t*, cairo_write_func_t, void*) = nullptr;
    void (*cairo_surface_destroy)(cairo_surface_t*) = nullptr;
    void (*cairo_surface_flush)(cairo_surface_t*) = nullptr;
    void (*cairo_surface_mark_dirty)(cairo_surface_t*) = nullptr;
    cairo_status_t (*cairo_surface_status)(cairo_surface_t*) = nullptr;
    int (*cairo_image_surface_get_width)(cairo_surface_t*) = nullptr;
    int (*cairo_image_surface_get_height)(cairo_surface_t*) = nullptr;
    int (*cairo_image_surface_get_stride)(cairo_surface_t*) = nullptr;
    unsigned char* (*cairo_image_surface_get_data)(cairo_surface_t*) = nullptr;
    cairo_format_t (*cairo_image_surface_get_format)(cairo_surface_t*) = nullptr;

    cairo_t* (*cairo_create)(cairo_surface_t*) = nullptr;
    void (*cairo_destroy)(cairo_t*) = nullptr;
    cairo_surface_t* (*cairo_get_target)(cairo_t*) = nullptr;
    void (*cairo_save)(cairo_t*) = nullptr;
    void (*cairo_restore)(cairo_t*) = nullptr;
    void (*cairo_set_source_rgba)(cairo_t*, double, double, double, double) = nullptr;
    void (*cairo_set_source_surface)(cairo_t*, cairo_surface_t*, double, double) = nullptr;
    void (*cairo_set_source)(cairo_t*, cairo_pattern_t*) = nullptr;
    cairo_pattern_t* (*cairo_pattern_create_for_surface)(cairo_surface_t*) = nullptr;
    cairo_pattern_t* (*cairo_pattern_create_linear)(double, double, double, double) = nullptr;
    void (*cairo_pattern_add_color_stop_rgba)(cairo_pattern_t*, double, double, double, double, double) = nullptr;
    void (*cairo_pattern_set_extend)(cairo_pattern_t*, cairo_extend_t) = nullptr;
    void (*cairo_pattern_set_filter)(cairo_pattern_t*, cairo_filter_t) = nullptr;
    void (*cairo_pattern_set_matrix)(cairo_pattern_t*, const cairo_matrix_t*) = nullptr;
    void (*cairo_pattern_destroy)(cairo_pattern_t*) = nullptr;
    void (*cairo_matrix_init)(cairo_matrix_t*, double, double, double, double, double, double) = nullptr;
    void (*cairo_matrix_init_identity)(cairo_matrix_t*) = nullptr;
    cairo_status_t (*cairo_matrix_invert)(cairo_matrix_t*) = nullptr;

    void (*cairo_set_operator)(cairo_t*, cairo_operator_t) = nullptr;
    void (*cairo_set_line_width)(cairo_t*, double) = nullptr;
    void (*cairo_set_line_cap)(cairo_t*, cairo_line_cap_t) = nullptr;
    void (*cairo_set_line_join)(cairo_t*, cairo_line_join_t) = nullptr;
    void (*cairo_set_dash)(cairo_t*, const double*, int, double) = nullptr;
    void (*cairo_set_antialias)(cairo_t*, cairo_antialias_t) = nullptr;
    void (*cairo_set_fill_rule)(cairo_t*, cairo_fill_rule_t) = nullptr;
    cairo_font_options_t* (*cairo_font_options_create)() = nullptr;
    void (*cairo_font_options_destroy)(cairo_font_options_t*) = nullptr;
    void (*cairo_font_options_set_antialias)(cairo_font_options_t*, cairo_antialias_t) = nullptr;
    void (*cairo_font_options_set_subpixel_order)(cairo_font_options_t*, cairo_subpixel_order_t) = nullptr;
    void (*cairo_font_options_set_hint_style)(cairo_font_options_t*, cairo_hint_style_t) = nullptr;
    void (*cairo_font_options_set_hint_metrics)(cairo_font_options_t*, cairo_hint_metrics_t) = nullptr;
    void (*cairo_set_font_options)(cairo_t*, const cairo_font_options_t*) = nullptr;
    void (*cairo_new_path)(cairo_t*) = nullptr;
    void (*cairo_move_to)(cairo_t*, double, double) = nullptr;
    void (*cairo_line_to)(cairo_t*, double, double) = nullptr;
    void (*cairo_curve_to)(cairo_t*, double, double, double, double, double, double) = nullptr;
    void (*cairo_arc)(cairo_t*, double, double, double, double, double) = nullptr;
    void (*cairo_close_path)(cairo_t*) = nullptr;
    void (*cairo_rectangle)(cairo_t*, double, double, double, double) = nullptr;
    void (*cairo_fill)(cairo_t*) = nullptr;
    void (*cairo_stroke)(cairo_t*) = nullptr;
    void (*cairo_clip)(cairo_t*) = nullptr;
    void (*cairo_paint)(cairo_t*) = nullptr;
    void (*cairo_paint_with_alpha)(cairo_t*, double) = nullptr;
    void (*cairo_translate)(cairo_t*, double, double) = nullptr;
    void (*cairo_scale)(cairo_t*, double, double) = nullptr;
    void (*cairo_transform)(cairo_t*, const cairo_matrix_t*) = nullptr;
    void (*cairo_identity_matrix)(cairo_t*) = nullptr;

    PangoLayout* (*pango_cairo_create_layout)(cairo_t*) = nullptr;
    void (*pango_cairo_update_layout)(cairo_t*, PangoLayout*) = nullptr;
    void (*pango_cairo_show_layout)(cairo_t*, PangoLayout*) = nullptr;
    void* (*pango_cairo_font_map_get_default)() = nullptr;
    void (*pango_fc_font_map_config_changed)(void*) = nullptr;

    PangoFontDescription* (*pango_font_description_new)() = nullptr;
    void (*pango_font_description_free)(PangoFontDescription*) = nullptr;
    void (*pango_font_description_set_family)(PangoFontDescription*, const char*) = nullptr;
    void (*pango_font_description_set_absolute_size)(PangoFontDescription*, double) = nullptr;
    void (*pango_font_description_set_weight)(PangoFontDescription*, int) = nullptr;
    void (*pango_font_description_set_style)(PangoFontDescription*, PangoStyle) = nullptr;

    void (*pango_layout_set_font_description)(PangoLayout*, const PangoFontDescription*) = nullptr;
    void (*pango_layout_set_text)(PangoLayout*, const char*, int) = nullptr;
    void (*pango_layout_set_width)(PangoLayout*, int) = nullptr;
    void (*pango_layout_set_height)(PangoLayout*, int) = nullptr;
    void (*pango_layout_set_wrap)(PangoLayout*, PangoWrapMode) = nullptr;
    void (*pango_layout_set_ellipsize)(PangoLayout*, PangoEllipsizeMode) = nullptr;
    void (*pango_layout_set_alignment)(PangoLayout*, PangoAlignment) = nullptr;
    void (*pango_layout_get_pixel_extents)(PangoLayout*, PangoRectangle*, PangoRectangle*) = nullptr;
    int (*pango_layout_get_line_count)(PangoLayout*) = nullptr;
    int (*pango_layout_xy_to_index)(PangoLayout*, int, int, int*, int*) = nullptr;
    void (*pango_layout_index_to_pos)(PangoLayout*, int, PangoRectangle*) = nullptr;
    void (*pango_layout_context_changed)(PangoLayout*) = nullptr;

    void (*g_object_unref)(void*) = nullptr;

    static CairoPangoApi& Get() {
        static CairoPangoApi api;
        static std::once_flag once;
        std::call_once(once, [&]() {
            api.libcairo = dlopen("libcairo.so.2", RTLD_LAZY | RTLD_LOCAL);
            api.libpangocairo = dlopen("libpangocairo-1.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            api.libpango = dlopen("libpango-1.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            api.libpangoft2 = dlopen("libpangoft2-1.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            api.libgobject = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            if (!api.libcairo) return;

            auto load = [](void* lib, auto& fn, const char* sym) {
                if (lib) fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(lib, sym));
            };

            load(api.libcairo, api.cairo_image_surface_create, "cairo_image_surface_create");
            load(api.libcairo, api.cairo_image_surface_create_for_data, "cairo_image_surface_create_for_data");
            load(api.libcairo, api.cairo_image_surface_create_from_png_stream, "cairo_image_surface_create_from_png_stream");
            load(api.libcairo, api.cairo_surface_write_to_png_stream, "cairo_surface_write_to_png_stream");
            load(api.libcairo, api.cairo_surface_destroy, "cairo_surface_destroy");
            load(api.libcairo, api.cairo_surface_flush, "cairo_surface_flush");
            load(api.libcairo, api.cairo_surface_mark_dirty, "cairo_surface_mark_dirty");
            load(api.libcairo, api.cairo_surface_status, "cairo_surface_status");
            load(api.libcairo, api.cairo_image_surface_get_width, "cairo_image_surface_get_width");
            load(api.libcairo, api.cairo_image_surface_get_height, "cairo_image_surface_get_height");
            load(api.libcairo, api.cairo_image_surface_get_stride, "cairo_image_surface_get_stride");
            load(api.libcairo, api.cairo_image_surface_get_data, "cairo_image_surface_get_data");
            load(api.libcairo, api.cairo_image_surface_get_format, "cairo_image_surface_get_format");

            load(api.libcairo, api.cairo_create, "cairo_create");
            load(api.libcairo, api.cairo_destroy, "cairo_destroy");
            load(api.libcairo, api.cairo_get_target, "cairo_get_target");
            load(api.libcairo, api.cairo_save, "cairo_save");
            load(api.libcairo, api.cairo_restore, "cairo_restore");
            load(api.libcairo, api.cairo_set_source_rgba, "cairo_set_source_rgba");
            load(api.libcairo, api.cairo_set_source_surface, "cairo_set_source_surface");
            load(api.libcairo, api.cairo_set_source, "cairo_set_source");
            load(api.libcairo, api.cairo_pattern_create_for_surface, "cairo_pattern_create_for_surface");
            load(api.libcairo, api.cairo_pattern_create_linear, "cairo_pattern_create_linear");
            load(api.libcairo, api.cairo_pattern_add_color_stop_rgba, "cairo_pattern_add_color_stop_rgba");
            load(api.libcairo, api.cairo_pattern_set_extend, "cairo_pattern_set_extend");
            load(api.libcairo, api.cairo_pattern_set_filter, "cairo_pattern_set_filter");
            load(api.libcairo, api.cairo_pattern_set_matrix, "cairo_pattern_set_matrix");
            load(api.libcairo, api.cairo_pattern_destroy, "cairo_pattern_destroy");
            load(api.libcairo, api.cairo_matrix_init, "cairo_matrix_init");
            load(api.libcairo, api.cairo_matrix_init_identity, "cairo_matrix_init_identity");
            load(api.libcairo, api.cairo_matrix_invert, "cairo_matrix_invert");

            load(api.libcairo, api.cairo_set_operator, "cairo_set_operator");
            load(api.libcairo, api.cairo_set_line_width, "cairo_set_line_width");
            load(api.libcairo, api.cairo_set_line_cap, "cairo_set_line_cap");
            load(api.libcairo, api.cairo_set_line_join, "cairo_set_line_join");
            load(api.libcairo, api.cairo_set_dash, "cairo_set_dash");
            load(api.libcairo, api.cairo_set_antialias, "cairo_set_antialias");
            load(api.libcairo, api.cairo_set_fill_rule, "cairo_set_fill_rule");
            load(api.libcairo, api.cairo_font_options_create, "cairo_font_options_create");
            load(api.libcairo, api.cairo_font_options_destroy, "cairo_font_options_destroy");
            load(api.libcairo, api.cairo_font_options_set_antialias, "cairo_font_options_set_antialias");
            load(api.libcairo, api.cairo_font_options_set_subpixel_order, "cairo_font_options_set_subpixel_order");
            load(api.libcairo, api.cairo_font_options_set_hint_style, "cairo_font_options_set_hint_style");
            load(api.libcairo, api.cairo_font_options_set_hint_metrics, "cairo_font_options_set_hint_metrics");
            load(api.libcairo, api.cairo_set_font_options, "cairo_set_font_options");
            load(api.libcairo, api.cairo_new_path, "cairo_new_path");
            load(api.libcairo, api.cairo_move_to, "cairo_move_to");
            load(api.libcairo, api.cairo_line_to, "cairo_line_to");
            load(api.libcairo, api.cairo_curve_to, "cairo_curve_to");
            load(api.libcairo, api.cairo_arc, "cairo_arc");
            load(api.libcairo, api.cairo_close_path, "cairo_close_path");
            load(api.libcairo, api.cairo_rectangle, "cairo_rectangle");
            load(api.libcairo, api.cairo_fill, "cairo_fill");
            load(api.libcairo, api.cairo_stroke, "cairo_stroke");
            load(api.libcairo, api.cairo_clip, "cairo_clip");
            load(api.libcairo, api.cairo_paint, "cairo_paint");
            load(api.libcairo, api.cairo_paint_with_alpha, "cairo_paint_with_alpha");
            load(api.libcairo, api.cairo_translate, "cairo_translate");
            load(api.libcairo, api.cairo_scale, "cairo_scale");
            load(api.libcairo, api.cairo_transform, "cairo_transform");
            load(api.libcairo, api.cairo_identity_matrix, "cairo_identity_matrix");

            load(api.libpangocairo, api.pango_cairo_create_layout, "pango_cairo_create_layout");
            load(api.libpangocairo, api.pango_cairo_update_layout, "pango_cairo_update_layout");
            load(api.libpangocairo, api.pango_cairo_show_layout, "pango_cairo_show_layout");
            load(api.libpangocairo, api.pango_cairo_font_map_get_default, "pango_cairo_font_map_get_default");
            load(api.libpangoft2, api.pango_fc_font_map_config_changed, "pango_fc_font_map_config_changed");

            load(api.libpango, api.pango_font_description_new, "pango_font_description_new");
            load(api.libpango, api.pango_font_description_free, "pango_font_description_free");
            load(api.libpango, api.pango_font_description_set_family, "pango_font_description_set_family");
            load(api.libpango, api.pango_font_description_set_absolute_size, "pango_font_description_set_absolute_size");
            load(api.libpango, api.pango_font_description_set_weight, "pango_font_description_set_weight");
            load(api.libpango, api.pango_font_description_set_style, "pango_font_description_set_style");

            load(api.libpango, api.pango_layout_set_font_description, "pango_layout_set_font_description");
            load(api.libpango, api.pango_layout_set_text, "pango_layout_set_text");
            load(api.libpango, api.pango_layout_set_width, "pango_layout_set_width");
            load(api.libpango, api.pango_layout_set_height, "pango_layout_set_height");
            load(api.libpango, api.pango_layout_set_wrap, "pango_layout_set_wrap");
            load(api.libpango, api.pango_layout_set_ellipsize, "pango_layout_set_ellipsize");
            load(api.libpango, api.pango_layout_set_alignment, "pango_layout_set_alignment");
            load(api.libpango, api.pango_layout_get_pixel_extents, "pango_layout_get_pixel_extents");
            load(api.libpango, api.pango_layout_get_line_count, "pango_layout_get_line_count");
            load(api.libpango, api.pango_layout_xy_to_index, "pango_layout_xy_to_index");
            load(api.libpango, api.pango_layout_index_to_pos, "pango_layout_index_to_pos");
            load(api.libpango, api.pango_layout_context_changed, "pango_layout_context_changed");

            load(api.libgobject, api.g_object_unref, "g_object_unref");

            api.loaded = (api.cairo_image_surface_create != nullptr &&
                          api.cairo_image_surface_create_for_data != nullptr &&
                          api.cairo_create != nullptr);
        });
        return api;
    }

    bool hasPango() const {
        return loaded && pango_cairo_create_layout && pango_cairo_show_layout &&
               pango_font_description_new && pango_layout_set_font_description &&
               pango_layout_set_text && pango_layout_get_pixel_extents && g_object_unref;
    }
};

struct FontconfigApi {
    void* lib = nullptr;
    int (*FcInit)() = nullptr;
    void* (*FcConfigGetCurrent)() = nullptr;
    int (*FcConfigAppFontAddFile)(void*, const unsigned char*) = nullptr;

    static FontconfigApi& Get() {
        static FontconfigApi api;
        static std::once_flag once;
        std::call_once(once, [&]() {
            api.lib = dlopen("libfontconfig.so.1", RTLD_LAZY | RTLD_LOCAL);
            if (!api.lib) return;
            api.FcInit = reinterpret_cast<int (*)()>(dlsym(api.lib, "FcInit"));
            api.FcConfigGetCurrent = reinterpret_cast<void* (*)()>(dlsym(api.lib, "FcConfigGetCurrent"));
            api.FcConfigAppFontAddFile = reinterpret_cast<int (*)(void*, const unsigned char*)>(
                dlsym(api.lib, "FcConfigAppFontAddFile"));
            if (api.FcInit) api.FcInit();
        });
        return api;
    }
};

struct GdkPixbufLoader;
struct GdkPixbuf;
struct GError;

struct GdkPixbufApi {
    void* libpixbuf = nullptr;
    void* libglib = nullptr;
    void* libgobject = nullptr;
    bool loaded = false;

    GdkPixbufLoader* (*gdk_pixbuf_loader_new)() = nullptr;
    int (*gdk_pixbuf_loader_write)(GdkPixbufLoader*, const unsigned char*, size_t, GError**) = nullptr;
    int (*gdk_pixbuf_loader_close)(GdkPixbufLoader*, GError**) = nullptr;
    GdkPixbuf* (*gdk_pixbuf_loader_get_pixbuf)(GdkPixbufLoader*) = nullptr;
    int (*gdk_pixbuf_get_width)(const GdkPixbuf*) = nullptr;
    int (*gdk_pixbuf_get_height)(const GdkPixbuf*) = nullptr;
    int (*gdk_pixbuf_get_rowstride)(const GdkPixbuf*) = nullptr;
    int (*gdk_pixbuf_get_n_channels)(const GdkPixbuf*) = nullptr;
    int (*gdk_pixbuf_get_has_alpha)(const GdkPixbuf*) = nullptr;
    unsigned char* (*gdk_pixbuf_get_pixels)(const GdkPixbuf*) = nullptr;
    GdkPixbuf* (*gdk_pixbuf_new_from_data)(const unsigned char*, int, int, int, int, int, int, void (*)(unsigned char*, void*), void*) = nullptr;
    int (*gdk_pixbuf_save_to_buffer)(GdkPixbuf*, char**, size_t*, const char*, GError**, ...) = nullptr;
    void (*g_free)(void*) = nullptr;
    void (*g_error_free)(GError*) = nullptr;
    void (*g_object_unref)(void*) = nullptr;

    static GdkPixbufApi& Get() {
        static GdkPixbufApi api;
        static std::once_flag once;
        std::call_once(once, [&]() {
            api.libpixbuf = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            api.libglib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            api.libgobject = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            if (!api.libpixbuf || !api.libglib || !api.libgobject) return;

            auto load = [](void* lib, auto& fn, const char* sym) {
                if (lib) fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(lib, sym));
            };
            load(api.libpixbuf, api.gdk_pixbuf_loader_new, "gdk_pixbuf_loader_new");
            load(api.libpixbuf, api.gdk_pixbuf_loader_write, "gdk_pixbuf_loader_write");
            load(api.libpixbuf, api.gdk_pixbuf_loader_close, "gdk_pixbuf_loader_close");
            load(api.libpixbuf, api.gdk_pixbuf_loader_get_pixbuf, "gdk_pixbuf_loader_get_pixbuf");
            load(api.libpixbuf, api.gdk_pixbuf_get_width, "gdk_pixbuf_get_width");
            load(api.libpixbuf, api.gdk_pixbuf_get_height, "gdk_pixbuf_get_height");
            load(api.libpixbuf, api.gdk_pixbuf_get_rowstride, "gdk_pixbuf_get_rowstride");
            load(api.libpixbuf, api.gdk_pixbuf_get_n_channels, "gdk_pixbuf_get_n_channels");
            load(api.libpixbuf, api.gdk_pixbuf_get_has_alpha, "gdk_pixbuf_get_has_alpha");
            load(api.libpixbuf, api.gdk_pixbuf_get_pixels, "gdk_pixbuf_get_pixels");
            load(api.libpixbuf, api.gdk_pixbuf_new_from_data, "gdk_pixbuf_new_from_data");
            load(api.libpixbuf, api.gdk_pixbuf_save_to_buffer, "gdk_pixbuf_save_to_buffer");
            load(api.libglib, api.g_free, "g_free");
            load(api.libglib, api.g_error_free, "g_error_free");
            load(api.libgobject, api.g_object_unref, "g_object_unref");

            api.loaded = (api.gdk_pixbuf_loader_new && api.gdk_pixbuf_loader_write &&
                          api.gdk_pixbuf_loader_close && api.gdk_pixbuf_loader_get_pixbuf &&
                          api.gdk_pixbuf_get_width && api.gdk_pixbuf_get_height &&
                          api.gdk_pixbuf_get_rowstride && api.gdk_pixbuf_get_n_channels &&
                          api.gdk_pixbuf_get_pixels && api.g_object_unref);
        });
        return api;
    }
};

// ============================================================================
// UTF-8 / wide index helpers
// ============================================================================

size_t Utf8CodepointBytes(uint32_t cp) {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

UINT32 Utf8ByteOffsetToWideIndex(const std::wstring& w, int byteOffset) {
    if (byteOffset <= 0) return 0;
    size_t bytes = 0;
    for (UINT32 i = 0; i < static_cast<UINT32>(w.size()); ++i) {
        if (bytes >= static_cast<size_t>(byteOffset)) return i;
        bytes += Utf8CodepointBytes(static_cast<uint32_t>(w[i]));
    }
    return static_cast<UINT32>(w.size());
}

int WideIndexToUtf8ByteOffset(const std::wstring& w, UINT32 wideIndex) {
    size_t bytes = 0;
    UINT32 limit = std::min<UINT32>(wideIndex, static_cast<UINT32>(w.size()));
    for (UINT32 i = 0; i < limit; ++i) {
        bytes += Utf8CodepointBytes(static_cast<uint32_t>(w[i]));
    }
    return static_cast<int>(bytes);
}

// ============================================================================
// Persistent scratch Cairo context for DPI-independent Pango layout metrics
// ============================================================================

std::atomic<uint64_t> g_fontConfigGeneration{1};

// Apply consistent subpixel-independent hinting and unhinted glyph metrics so UI fonts
// like Inter retain clean kerning and uniform stroke weight on Linux/Wayland image surfaces.
void ApplyTextFontOptions(cairo_t* cr, D2D1_TEXT_ANTIALIAS_MODE textMode) {
    auto& api = CairoPangoApi::Get();
    if (!cr || !api.cairo_set_font_options || !api.cairo_font_options_create ||
        !api.cairo_font_options_destroy) {
        return;
    }

    auto makeOptions = [&](cairo_antialias_t aa) -> cairo_font_options_t* {
        cairo_font_options_t* opts = api.cairo_font_options_create();
        if (!opts) return nullptr;
        if (api.cairo_font_options_set_antialias) {
            api.cairo_font_options_set_antialias(opts, aa);
        }
        if (api.cairo_font_options_set_hint_style) {
            api.cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_SLIGHT);
        }
        if (api.cairo_font_options_set_hint_metrics) {
            api.cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_OFF);
        }
        return opts;
    };

    static cairo_font_options_t* s_aliasedOpts = makeOptions(CAIRO_ANTIALIAS_NONE);
    static cairo_font_options_t* s_smoothOpts = makeOptions(CAIRO_ANTIALIAS_GRAY);

    cairo_font_options_t* chosen =
        (textMode == D2D1_TEXT_ANTIALIAS_MODE_ALIASED) ? s_aliasedOpts : s_smoothOpts;
    if (chosen) {
        api.cairo_set_font_options(cr, chosen);
    }
}

cairo_t* GetScratchMetricCairo() {
    auto& api = CairoPangoApi::Get();
    if (!api.loaded) return nullptr;
    static cairo_surface_t* s_surf = nullptr;
    static cairo_t* s_cr = nullptr;
    static std::once_flag once;
    std::call_once(once, [&]() {
        s_surf = api.cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        if (s_surf) {
            s_cr = api.cairo_create(s_surf);
            if (s_cr) ApplyTextFontOptions(s_cr, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
    });
    return s_cr;
}

inline bool HasBoundedLayoutWidth(const IDWriteTextLayout* tl) {
    return tl && tl->m_maxWidth > 0.0f && tl->m_maxWidth < 1e6f;
}

inline bool UsesBoundedPangoWidth(const IDWriteTextLayout* tl) {
    if (!tl || !HasBoundedLayoutWidth(tl)) return false;
    return (tl->m_wordWrapping != DWRITE_WORD_WRAPPING_NO_WRAP) ||
           (tl->m_trimming.granularity != DWRITE_TRIMMING_GRANULARITY_NONE);
}

inline DWRITE_TEXT_ALIGNMENT ResolveEffectiveTextAlignment(const IDWriteTextLayout* tl) {
    if (!tl) return DWRITE_TEXT_ALIGNMENT_LEADING;
    const bool rtl = (tl->m_readingDirection == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    if (tl->m_textAlignment == DWRITE_TEXT_ALIGNMENT_LEADING) {
        return rtl ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
    }
    if (tl->m_textAlignment == DWRITE_TEXT_ALIGNMENT_TRAILING) {
        return rtl ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_TRAILING;
    }
    return tl->m_textAlignment;
}

inline double ComputeUnboundedHorizontalOffset(const IDWriteTextLayout* tl, int logicalWidth) {
    if (!tl || UsesBoundedPangoWidth(tl) || !HasBoundedLayoutWidth(tl)) return 0.0;
    const DWRITE_TEXT_ALIGNMENT effAlign = ResolveEffectiveTextAlignment(tl);
    if (effAlign == DWRITE_TEXT_ALIGNMENT_CENTER) {
        return (static_cast<double>(tl->m_maxWidth) - logicalWidth) * 0.5;
    }
    if (effAlign == DWRITE_TEXT_ALIGNMENT_TRAILING) {
        return static_cast<double>(tl->m_maxWidth) - logicalWidth;
    }
    return 0.0;
}

PangoLayout* EnsurePangoLayout(const IDWriteTextLayout* tl, cairo_t* targetCr) {
    auto& api = CairoPangoApi::Get();
    if (!api.hasPango() || !tl) return nullptr;

    cairo_t* createCr = GetScratchMetricCairo();
    if (!createCr) createCr = targetCr;
    if (!createCr) return nullptr;

    const uint64_t fontGen = g_fontConfigGeneration.load(std::memory_order_relaxed);
    bool valid = (tl->m_cachedPangoLayout != nullptr &&
                  tl->m_cachedFontGeneration == fontGen &&
                  tl->m_cachedText == tl->m_text &&
                  tl->m_cachedFamily == tl->m_familyName &&
                  tl->m_cachedFontSize == tl->m_fontSize &&
                  tl->m_cachedMaxWidth == tl->m_maxWidth &&
                  tl->m_cachedWeight == tl->m_weight &&
                  tl->m_cachedStyle == tl->m_style &&
                  tl->m_cachedAlign == tl->m_textAlignment &&
                  tl->m_cachedReadingDirection == tl->m_readingDirection &&
                  tl->m_cachedWrapping == tl->m_wordWrapping &&
                  tl->m_cachedTrimming == tl->m_trimming.granularity);

    PangoLayout* layout = static_cast<PangoLayout*>(tl->m_cachedPangoLayout);
    if (!valid) {
        if (!layout) {
            layout = api.pango_cairo_create_layout(createCr);
            if (!layout) return nullptr;
            tl->m_cachedPangoLayout = layout;
        } else if (tl->m_cachedFontGeneration != fontGen && api.pango_layout_context_changed) {
            api.pango_layout_context_changed(layout);
        }

        std::string utf8 = LinuxWideToUtf8(tl->m_text);
        std::string family = tl->m_familyName.empty() ? "Sans" : LinuxWideToUtf8(tl->m_familyName);

        PangoFontDescription* desc = api.pango_font_description_new();
        api.pango_font_description_set_family(desc, family.c_str());
        api.pango_font_description_set_absolute_size(desc, std::max(1.0f, tl->m_fontSize) * kPangoScale);
        if (api.pango_font_description_set_weight) {
            api.pango_font_description_set_weight(desc, static_cast<int>(tl->m_weight));
        }
        if (api.pango_font_description_set_style) {
            PangoStyle ps = PANGO_STYLE_NORMAL;
            if (tl->m_style == DWRITE_FONT_STYLE_ITALIC) ps = PANGO_STYLE_ITALIC;
            else if (tl->m_style == DWRITE_FONT_STYLE_OBLIQUE) ps = PANGO_STYLE_OBLIQUE;
            api.pango_font_description_set_style(desc, ps);
        }

        api.pango_layout_set_font_description(layout, desc);
        api.pango_font_description_free(desc);
        api.pango_layout_set_text(layout, utf8.data(), static_cast<int>(utf8.size()));

        const bool useBoundedWidth = UsesBoundedPangoWidth(tl);
        if (api.pango_layout_set_width) {
            api.pango_layout_set_width(
                layout,
                useBoundedWidth ? static_cast<int>(std::max(1.0f, tl->m_maxWidth) * kPangoScale) : -1);
        }
        if (api.pango_layout_set_wrap) {
            api.pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        }
        if (api.pango_layout_set_ellipsize) {
            const bool trim = (tl->m_trimming.granularity != DWRITE_TRIMMING_GRANULARITY_NONE);
            api.pango_layout_set_ellipsize(layout, trim ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
            if (api.pango_layout_set_height) {
                if (trim && tl->m_wordWrapping == DWRITE_WORD_WRAPPING_NO_WRAP) {
                    api.pango_layout_set_height(layout, 0);
                } else if (trim && tl->m_maxHeight > 0.0f && tl->m_maxHeight < 1e6f) {
                    api.pango_layout_set_height(layout, static_cast<int>(tl->m_maxHeight * kPangoScale));
                } else {
                    api.pango_layout_set_height(layout, -1);
                }
            }
        }
        if (api.pango_layout_set_alignment) {
            const DWRITE_TEXT_ALIGNMENT effAlign = ResolveEffectiveTextAlignment(tl);
            PangoAlignment pa = PANGO_ALIGN_LEFT;
            if (effAlign == DWRITE_TEXT_ALIGNMENT_CENTER) pa = PANGO_ALIGN_CENTER;
            else if (effAlign == DWRITE_TEXT_ALIGNMENT_TRAILING) pa = PANGO_ALIGN_RIGHT;
            api.pango_layout_set_alignment(layout, pa);
        }

        tl->m_cachedText = tl->m_text;
        tl->m_cachedFamily = tl->m_familyName;
        tl->m_cachedFontSize = tl->m_fontSize;
        tl->m_cachedMaxWidth = tl->m_maxWidth;
        tl->m_cachedWeight = tl->m_weight;
        tl->m_cachedStyle = tl->m_style;
        tl->m_cachedAlign = tl->m_textAlignment;
        tl->m_cachedReadingDirection = tl->m_readingDirection;
        tl->m_cachedWrapping = tl->m_wordWrapping;
        tl->m_cachedTrimming = tl->m_trimming.granularity;
        tl->m_cachedFontGeneration = fontGen;
    }

    if (targetCr && api.pango_cairo_update_layout) {
        api.pango_cairo_update_layout(targetCr, layout);
    }
    return layout;
}

// ============================================================================
// Cairo surface / context caching & state helpers
// ============================================================================

cairo_surface_t* EnsureCairoSurfaceForBitmap(ID2D1Bitmap* bmp) {
    auto& api = CairoPangoApi::Get();
    if (!api.loaded || !bmp || !bmp->HasPixels() || bmp->m_width == 0 || bmp->m_height == 0) {
        return nullptr;
    }
    uint8_t* ptr = bmp->PixelData();
    cairo_surface_t* surf = static_cast<cairo_surface_t*>(bmp->m_cachedCairoSurface);
    if (surf &&
        bmp->m_cachedSurfacePtr == ptr &&
        bmp->m_cachedSurfaceW == bmp->m_width &&
        bmp->m_cachedSurfaceH == bmp->m_height) {
        if (bmp->m_dirtyForCairo && api.cairo_surface_mark_dirty) {
            api.cairo_surface_mark_dirty(surf);
            bmp->m_dirtyForCairo = false;
        }
        return surf;
    }
    if (surf && api.cairo_surface_destroy) {
        api.cairo_surface_destroy(surf);
        bmp->m_cachedCairoSurface = nullptr;
    }
    surf = api.cairo_image_surface_create_for_data(
        ptr,
        CAIRO_FORMAT_ARGB32,
        static_cast<int>(bmp->m_width),
        static_cast<int>(bmp->m_height),
        static_cast<int>(bmp->m_width * 4));
    bmp->m_cachedCairoSurface = surf;
    bmp->m_cachedSurfacePtr = ptr;
    bmp->m_cachedSurfaceW = bmp->m_width;
    bmp->m_cachedSurfaceH = bmp->m_height;
    bmp->m_dirtyForCairo = false;
    return surf;
}

cairo_t* EnsureTargetCairo(ID2D1RenderTarget* rt) {
    auto& api = CairoPangoApi::Get();
    if (!api.loaded || !rt || !rt->m_targetBitmap) return nullptr;
    cairo_surface_t* surf = EnsureCairoSurfaceForBitmap(rt->m_targetBitmap.Get());
    if (!surf) return nullptr;

    cairo_t* cr = static_cast<cairo_t*>(rt->m_cachedCr);
    if (cr && rt->m_cachedCrBitmap == rt->m_targetBitmap.Get() &&
        (!api.cairo_get_target || api.cairo_get_target(cr) == surf)) {
        return cr;
    }
    if (cr && api.cairo_destroy) {
        api.cairo_destroy(cr);
        rt->m_cachedCr = nullptr;
        rt->m_cachedCrBitmap = nullptr;
    }
    cr = api.cairo_create(surf);
    rt->m_cachedCr = cr;
    rt->m_cachedCrBitmap = rt->m_targetBitmap.Get();
    return cr;
}

void GetDpiScale(const ID2D1RenderTarget* rt, double& sx, double& sy) {
    sx = (rt && rt->m_dpiX > 0.0f) ? (rt->m_dpiX / 96.0) : 1.0;
    sy = (rt && rt->m_dpiY > 0.0f) ? (rt->m_dpiY / 96.0) : 1.0;
}

void ApplyClipList(cairo_t* cr, const std::vector<D2D1_RECT_F>& clips) {
    auto& api = CairoPangoApi::Get();
    for (const auto& clip : clips) {
        api.cairo_rectangle(cr, clip.left, clip.top,
                            std::max(0.0f, clip.right - clip.left),
                            std::max(0.0f, clip.bottom - clip.top));
        api.cairo_clip(cr);
    }
}

void ApplyRTState(cairo_t* cr, const ID2D1RenderTarget* rt) {
    auto& api = CairoPangoApi::Get();
    double sx = 1.0, sy = 1.0;
    GetDpiScale(rt, sx, sy);

    if (api.cairo_identity_matrix) api.cairo_identity_matrix(cr);
    if (rt->m_pixelOriginX != 0 || rt->m_pixelOriginY != 0) {
        api.cairo_translate(cr, -static_cast<double>(rt->m_pixelOriginX), -static_cast<double>(rt->m_pixelOriginY));
    }
    api.cairo_scale(cr, sx, sy);

    if (api.cairo_transform) {
        cairo_matrix_t mat{
            rt->m_transform.m11, rt->m_transform.m12,
            rt->m_transform.m21, rt->m_transform.m22,
            rt->m_transform.dx,  rt->m_transform.dy
        };
        api.cairo_transform(cr, &mat);
    }
    if (api.cairo_set_antialias) {
        api.cairo_set_antialias(cr, rt->m_antialiasMode == D2D1_ANTIALIAS_MODE_ALIASED
                                        ? CAIRO_ANTIALIAS_NONE
                                        : CAIRO_ANTIALIAS_DEFAULT);
    }
    ApplyTextFontOptions(cr, rt->m_textAntialiasMode);
    ApplyClipList(cr, rt->m_clipStack);
}

void ApplyBrushToCairo(cairo_t* cr, ID2D1Brush* brush) {
    auto& api = CairoPangoApi::Get();
    if (!brush) {
        api.cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        return;
    }

    if (auto* sb = dynamic_cast<ID2D1SolidColorBrush*>(brush)) {
        api.cairo_set_source_rgba(cr, sb->m_color.r, sb->m_color.g, sb->m_color.b,
                                  sb->m_color.a * sb->m_opacity);
        return;
    }

    if (auto* lg = dynamic_cast<ID2D1LinearGradientBrush*>(brush)) {
        if (api.cairo_pattern_create_linear && api.cairo_pattern_add_color_stop_rgba &&
            api.cairo_set_source && api.cairo_pattern_destroy) {
            cairo_pattern_t* pat = api.cairo_pattern_create_linear(
                lg->m_start.x, lg->m_start.y,
                lg->m_end.x, lg->m_end.y);
            if (pat) {
                if (lg->m_stops) {
                    for (const auto& s : lg->m_stops->m_stops) {
                        api.cairo_pattern_add_color_stop_rgba(
                            pat, s.position, s.color.r, s.color.g, s.color.b,
                            s.color.a * lg->m_opacity);
                    }
                }
                if (api.cairo_pattern_set_extend) api.cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
                if (api.cairo_pattern_set_matrix && api.cairo_matrix_invert) {
                    cairo_matrix_t m{
                        lg->m_transform.m11, lg->m_transform.m12,
                        lg->m_transform.m21, lg->m_transform.m22,
                        lg->m_transform.dx,  lg->m_transform.dy
                    };
                    if (api.cairo_matrix_invert(&m) == CAIRO_STATUS_SUCCESS) {
                        api.cairo_pattern_set_matrix(pat, &m);
                    }
                }
                api.cairo_set_source(cr, pat);
                api.cairo_pattern_destroy(pat);
                return;
            }
        }
        if (lg->m_stops && !lg->m_stops->m_stops.empty()) {
            const auto& c = lg->m_stops->m_stops[0].color;
            api.cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * lg->m_opacity);
            return;
        }
    }

    if (auto* bb = dynamic_cast<ID2D1BitmapBrush*>(brush)) {
        if (bb->m_bitmap && api.cairo_pattern_create_for_surface &&
            api.cairo_set_source && api.cairo_pattern_destroy) {
            if (cairo_surface_t* surf = EnsureCairoSurfaceForBitmap(bb->m_bitmap.Get())) {
                if (api.cairo_surface_flush) api.cairo_surface_flush(surf);
                cairo_pattern_t* pat = api.cairo_pattern_create_for_surface(surf);
                if (pat) {
                    if (api.cairo_pattern_set_extend) {
                        api.cairo_pattern_set_extend(
                            pat,
                            bb->m_props.extendModeX == D2D1_EXTEND_MODE_WRAP
                                ? CAIRO_EXTEND_REPEAT
                                : CAIRO_EXTEND_PAD);
                    }
                    if (api.cairo_pattern_set_matrix && api.cairo_matrix_invert) {
                        cairo_matrix_t m{
                            bb->m_transform.m11, bb->m_transform.m12,
                            bb->m_transform.m21, bb->m_transform.m22,
                            bb->m_transform.dx,  bb->m_transform.dy
                        };
                        if (api.cairo_matrix_invert(&m) == CAIRO_STATUS_SUCCESS) {
                            api.cairo_pattern_set_matrix(pat, &m);
                        }
                    }
                    api.cairo_set_source(cr, pat);
                    api.cairo_pattern_destroy(pat);
                    return;
                }
            }
        }
    }

    api.cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, brush->m_opacity);
}

void ApplyStrokeStyle(cairo_t* /*cr*/, ID2D1StrokeStyle* /*style*/) {}

void EmitRoundedRectPath(cairo_t* cr, const D2D1_ROUNDED_RECT& rr) {
    auto& api = CairoPangoApi::Get();
    double l = rr.rect.left, t = rr.rect.top, r = rr.rect.right, b = rr.rect.bottom;
    double w = std::max(0.0, r - l), h = std::max(0.0, b - t);
    double rad = std::min(std::max(0.0, static_cast<double>(rr.radiusX)), std::min(w, h) * 0.5);
    api.cairo_new_path(cr);
    if (rad <= 0.01) {
        api.cairo_rectangle(cr, l, t, w, h);
    } else {
        api.cairo_arc(cr, r - rad, t + rad, rad, -kPi * 0.5, 0.0);
        api.cairo_arc(cr, r - rad, b - rad, rad, 0.0, kPi * 0.5);
        api.cairo_arc(cr, l + rad, b - rad, rad, kPi * 0.5, kPi);
        api.cairo_arc(cr, l + rad, t + rad, rad, kPi, kPi * 1.5);
        api.cairo_close_path(cr);
    }
}

void EmitEllipticalArcToCairo(cairo_t* cr, double x1, double y1, const D2D1_ARC_SEGMENT& arc) {
    auto& api = CairoPangoApi::Get();
    double x2 = arc.point.x;
    double y2 = arc.point.y;
    if (std::abs(x1 - x2) < 1e-6 && std::abs(y1 - y2) < 1e-6) return;

    double rx = std::abs(static_cast<double>(arc.size.width));
    double ry = std::abs(static_cast<double>(arc.size.height));
    if (rx < 1e-6 || ry < 1e-6) {
        api.cairo_line_to(cr, x2, y2);
        return;
    }

    double phi = static_cast<double>(arc.rotationAngle) * (kPi / 180.0);
    double cosPhi = std::cos(phi);
    double sinPhi = std::sin(phi);

    double dx2 = (x1 - x2) * 0.5;
    double dy2 = (y1 - y2) * 0.5;
    double x1p = cosPhi * dx2 + sinPhi * dy2;
    double y1p = -sinPhi * dx2 + cosPhi * dy2;

    double rx2 = rx * rx;
    double ry2 = ry * ry;
    double x1p2 = x1p * x1p;
    double y1p2 = y1p * y1p;
    double lambda = x1p2 / rx2 + y1p2 / ry2;
    if (lambda > 1.0) {
        double s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
        rx2 = rx * rx;
        ry2 = ry * ry;
    }

    bool largeArc = (arc.arcSize == D2D1_ARC_SIZE_LARGE);
    bool sweepCw = (arc.sweepDirection == D2D1_SWEEP_DIRECTION_CLOCKWISE);

    double num = std::max(0.0, rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2);
    double den = rx2 * y1p2 + ry2 * x1p2;
    double factor = (den > 1e-12) ? std::sqrt(num / den) : 0.0;
    if (largeArc == sweepCw) factor = -factor;

    double cxp = factor * (rx * y1p / ry);
    double cyp = factor * (-ry * x1p / rx);

    double cx = cosPhi * cxp - sinPhi * cyp + (x1 + x2) * 0.5;
    double cy = sinPhi * cxp + cosPhi * cyp + (y1 + y2) * 0.5;

    auto vecAngle = [](double ux, double uy, double vx, double vy) {
        double dot = ux * vx + uy * vy;
        double len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        double cosv = (len > 1e-12) ? std::max(-1.0, std::min(1.0, dot / len)) : 1.0;
        double ang = std::acos(cosv);
        return (ux * vy - uy * vx < 0.0) ? -ang : ang;
    };

    double ux = (x1p - cxp) / rx;
    double uy = (y1p - cyp) / ry;
    double vx = (-x1p - cxp) / rx;
    double vy = (-y1p - cyp) / ry;

    double theta1 = vecAngle(1.0, 0.0, ux, uy);
    double dtheta = vecAngle(ux, uy, vx, vy);
    if (!sweepCw && dtheta > 0.0) dtheta -= 2.0 * kPi;
    if (sweepCw && dtheta < 0.0) dtheta += 2.0 * kPi;

    int segs = std::max(1, static_cast<int>(std::ceil(std::abs(dtheta) / (kPi * 0.5))));
    double delta = dtheta / segs;
    double alpha = (4.0 / 3.0) * std::tan(delta * 0.25);

    double t = theta1;
    for (int i = 0; i < segs; ++i) {
        double tNext = t + delta;
        double cos1 = std::cos(t), sin1 = std::sin(t);
        double cos2 = std::cos(tNext), sin2 = std::sin(tNext);

        double ep1x = cos1 - alpha * sin1;
        double ep1y = sin1 + alpha * cos1;
        double ep2x = cos2 + alpha * sin2;
        double ep2y = sin2 - alpha * cos2;

        auto mapPt = [&](double ex, double ey, double& ox, double& oy) {
            double sxpt = rx * ex;
            double sypt = ry * ey;
            ox = cx + cosPhi * sxpt - sinPhi * sypt;
            oy = cy + sinPhi * sxpt + cosPhi * sypt;
        };

        double c1x, c1y, c2x, c2y, ex, ey;
        mapPt(ep1x, ep1y, c1x, c1y);
        mapPt(ep2x, ep2y, c2x, c2y);
        if (i + 1 == segs) {
            ex = x2;
            ey = y2;
        } else {
            mapPt(cos2, sin2, ex, ey);
        }
        api.cairo_curve_to(cr, c1x, c1y, c2x, c2y, ex, ey);
        t = tNext;
    }
}

void EmitGeometryToCairo(cairo_t* cr, ID2D1Geometry* geom) {
    auto& api = CairoPangoApi::Get();
    if (!geom) return;
    if (geom->m_isRoundedRect) {
        EmitRoundedRectPath(cr, geom->m_roundedRect);
        return;
    }
    api.cairo_new_path(cr);
    double curX = 0.0, curY = 0.0;
    double figStartX = 0.0, figStartY = 0.0;
    for (const auto& cmd : geom->m_cmds) {
        switch (cmd.type) {
            case LinuxPathCmd::Begin:
                api.cairo_move_to(cr, cmd.p1.x, cmd.p1.y);
                curX = figStartX = cmd.p1.x;
                curY = figStartY = cmd.p1.y;
                break;
            case LinuxPathCmd::Line:
                api.cairo_line_to(cr, cmd.p1.x, cmd.p1.y);
                curX = cmd.p1.x;
                curY = cmd.p1.y;
                break;
            case LinuxPathCmd::Bezier:
                api.cairo_curve_to(cr,
                                   cmd.p1.x, cmd.p1.y,
                                   cmd.p2.x, cmd.p2.y,
                                   cmd.p3.x, cmd.p3.y);
                curX = cmd.p3.x;
                curY = cmd.p3.y;
                break;
            case LinuxPathCmd::Arc:
                EmitEllipticalArcToCairo(cr, curX, curY, cmd.arc);
                curX = cmd.arc.point.x;
                curY = cmd.arc.point.y;
                break;
            case LinuxPathCmd::End:
                if (cmd.endType == D2D1_FIGURE_END_CLOSED) {
                    api.cairo_close_path(cr);
                    curX = figStartX;
                    curY = figStartY;
                }
                break;
        }
    }
}

D2D1_POINT_2F TransformPt(const D2D1_MATRIX_3X2_F& m, FLOAT x, FLOAT y) {
    return D2D1::Point2F(m.m11 * x + m.m21 * y + m.dx, m.m12 * x + m.m22 * y + m.dy);
}

Microsoft::WRL::ComPtr<ID2D1Bitmap1> AcquireLayerBitmap(ID2D1RenderTarget* rt, UINT32 w, UINT32 h) {
    if (!rt) return nullptr;
    for (auto it = rt->m_layerPool.begin(); it != rt->m_layerPool.end(); ++it) {
        if ((*it) && (*it)->m_width == w && (*it)->m_height == h) {
            auto bmp = *it;
            rt->m_layerPool.erase(it);
            if (bmp->HasPixels()) {
                std::memset(bmp->PixelData(), 0, bmp->PixelByteCount());
                bmp->m_dirtyForCairo = true;
            }
            return bmp;
        }
    }
    return Microsoft::WRL::ComPtr<ID2D1Bitmap1>(new ID2D1Bitmap1(w, h));
}

void RecycleLayerBitmap(ID2D1RenderTarget* rt, Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp) {
    if (!rt || !bmp) return;
    if (rt->m_layerPool.size() < 8) {
        rt->m_layerPool.push_back(std::move(bmp));
    }
}

// ============================================================================
// Image Decode / Encode Helpers
// ============================================================================

struct MemReadState {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

cairo_status_t CairoMemRead(void* closure, unsigned char* data, unsigned int length) {
    auto* st = static_cast<MemReadState*>(closure);
    if (!st || st->pos + length > st->size) {
        return static_cast<cairo_status_t>(1); // CAIRO_STATUS_READ_ERROR
    }
    std::memcpy(data, st->data + st->pos, length);
    st->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t CairoVecWrite(void* closure, const unsigned char* data, unsigned int length) {
    auto* out = static_cast<std::vector<uint8_t>*>(closure);
    if (!out) return static_cast<cairo_status_t>(1);
    out->insert(out->end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
}

bool DecodePngStreamToBGRA(const uint8_t* bytes, size_t size,
                           std::vector<uint8_t>& outBGRA, UINT& outW, UINT& outH, bool& outPremul) {
    auto& api = CairoPangoApi::Get();
    if (!api.loaded || !api.cairo_image_surface_create_from_png_stream ||
        !api.cairo_surface_destroy || !api.cairo_image_surface_get_width ||
        !api.cairo_image_surface_get_height || !api.cairo_image_surface_get_stride ||
        !api.cairo_image_surface_get_data) {
        return false;
    }

    MemReadState st{bytes, size, 0};
    cairo_surface_t* surf = api.cairo_image_surface_create_from_png_stream(CairoMemRead, &st);
    if (!surf) return false;
    if (api.cairo_surface_status && api.cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        api.cairo_surface_destroy(surf);
        return false;
    }

    int w = api.cairo_image_surface_get_width(surf);
    int h = api.cairo_image_surface_get_height(surf);
    int stride = api.cairo_image_surface_get_stride(surf);
    unsigned char* src = api.cairo_image_surface_get_data(surf);
    cairo_format_t fmt = api.cairo_image_surface_get_format ? api.cairo_image_surface_get_format(surf) : CAIRO_FORMAT_ARGB32;

    if (w <= 0 || h <= 0 || !src) {
        api.cairo_surface_destroy(surf);
        return false;
    }

    outW = static_cast<UINT>(w);
    outH = static_cast<UINT>(h);
    outBGRA.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = src + y * stride;
        uint8_t* drow = outBGRA.data() + static_cast<size_t>(y) * w * 4;
        std::memcpy(drow, srow, static_cast<size_t>(w) * 4);
        if (fmt == CAIRO_FORMAT_RGB24) {
            for (int x = 0; x < w; ++x) drow[x * 4 + 3] = 255;
        }
    }
    outPremul = (fmt == CAIRO_FORMAT_ARGB32);
    api.cairo_surface_destroy(surf);
    return true;
}

bool DecodeGdkPixbufToBGRA(const uint8_t* bytes, size_t size,
                           std::vector<uint8_t>& outBGRA, UINT& outW, UINT& outH) {
    auto& api = GdkPixbufApi::Get();
    if (!api.loaded) return false;

    GdkPixbufLoader* loader = api.gdk_pixbuf_loader_new();
    if (!loader) return false;

    GError* err = nullptr;
    if (!api.gdk_pixbuf_loader_write(loader, bytes, size, &err)) {
        if (err && api.g_error_free) api.g_error_free(err);
        api.gdk_pixbuf_loader_close(loader, nullptr);
        api.g_object_unref(loader);
        return false;
    }
    if (!api.gdk_pixbuf_loader_close(loader, &err)) {
        if (err && api.g_error_free) api.g_error_free(err);
        api.g_object_unref(loader);
        return false;
    }

    GdkPixbuf* pb = api.gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pb) {
        api.g_object_unref(loader);
        return false;
    }

    int w = api.gdk_pixbuf_get_width(pb);
    int h = api.gdk_pixbuf_get_height(pb);
    int stride = api.gdk_pixbuf_get_rowstride(pb);
    int channels = api.gdk_pixbuf_get_n_channels(pb);
    unsigned char* src = api.gdk_pixbuf_get_pixels(pb);
    if (w <= 0 || h <= 0 || !src || (channels != 3 && channels != 4)) {
        api.g_object_unref(loader);
        return false;
    }

    outW = static_cast<UINT>(w);
    outH = static_cast<UINT>(h);
    outBGRA.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = src + y * stride;
        uint8_t* drow = outBGRA.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t r = srow[x * channels + 0];
            uint8_t g = srow[x * channels + 1];
            uint8_t b = srow[x * channels + 2];
            uint8_t a = (channels == 4) ? srow[x * channels + 3] : 255;
            drow[x * 4 + 0] = b;
            drow[x * 4 + 1] = g;
            drow[x * 4 + 2] = r;
            drow[x * 4 + 3] = a;
        }
    }
    api.g_object_unref(loader);
    return true;
}

bool DecodeImageBytesToBGRA(const uint8_t* bytes, size_t size,
                            std::vector<uint8_t>& outBGRA, UINT& outW, UINT& outH, bool& outPremul) {
    if (!bytes || size < 8) return false;
    static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(bytes, kPngSig, 8) == 0) {
        if (DecodePngStreamToBGRA(bytes, size, outBGRA, outW, outH, outPremul)) {
            return true;
        }
    }
    if (DecodeGdkPixbufToBGRA(bytes, size, outBGRA, outW, outH)) {
        outPremul = false;
        return true;
    }
    return false;
}

std::vector<uint8_t> ReadStreamBytes(IUnknown* pIStream) {
    std::vector<uint8_t> data;
    if (!pIStream) return data;
    if (auto* st = dynamic_cast<IStream*>(pIStream)) {
        data = st->m_data;
    }
    return data;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::vector<uint8_t> data;
    std::string posixPath = LinuxWinPathToPosix(path);
    std::ifstream f(posixPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return data;
    std::streamsize sz = f.tellg();
    if (sz <= 0) return data;
    f.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(data.data()), sz)) {
        data.clear();
    }
    return data;
}

} // namespace

// ============================================================================
// IDWriteFontSetBuilder1
// ============================================================================

HRESULT IDWriteFontSetBuilder1::AddFontFile(IDWriteFontFile* fontFile) {
    if (!fontFile) return E_INVALIDARG;
    if (!fontFile->m_filePath.empty()) {
        std::string posixPath = LinuxWinPathToPosix(fontFile->m_filePath);
        static std::mutex s_fcMtx;
        static std::unordered_set<std::string> s_loadedFonts;
        std::lock_guard<std::mutex> lock(s_fcMtx);
        if (s_loadedFonts.insert(posixPath).second) {
            auto& fc = FontconfigApi::Get();
            if (fc.FcConfigGetCurrent && fc.FcConfigAppFontAddFile) {
                if (void* cfg = fc.FcConfigGetCurrent()) {
                    if (fc.FcConfigAppFontAddFile(
                            cfg, reinterpret_cast<const unsigned char*>(posixPath.c_str()))) {
                        g_fontConfigGeneration.fetch_add(1, std::memory_order_relaxed);
                        auto& cp = CairoPangoApi::Get();
                        if (cp.pango_cairo_font_map_get_default && cp.pango_fc_font_map_config_changed) {
                            if (void* fm = cp.pango_cairo_font_map_get_default()) {
                                cp.pango_fc_font_map_config_changed(fm);
                            }
                        }
                    }
                }
            }
        }
    }
    return S_OK;
}

// ============================================================================
// IDWriteTextLayout
// ============================================================================

IDWriteTextLayout::~IDWriteTextLayout() {
    if (m_cachedPangoLayout) {
        auto& api = CairoPangoApi::Get();
        if (api.g_object_unref) api.g_object_unref(m_cachedPangoLayout);
        m_cachedPangoLayout = nullptr;
    }
}

HRESULT IDWriteTextLayout::GetMetrics(DWRITE_TEXT_METRICS* textMetrics) {
    if (!textMetrics) return E_POINTER;
    std::memset(textMetrics, 0, sizeof(*textMetrics));

    if (PangoLayout* layout = EnsurePangoLayout(this, GetScratchMetricCairo())) {
        auto& api = CairoPangoApi::Get();
        PangoRectangle logical{};
        api.pango_layout_get_pixel_extents(layout, nullptr, &logical);
        textMetrics->left = static_cast<FLOAT>(logical.x + ComputeUnboundedHorizontalOffset(this, logical.width));
        textMetrics->top = static_cast<FLOAT>(logical.y);
        textMetrics->width = static_cast<FLOAT>(logical.width);
        textMetrics->widthIncludingTrailingWhitespace = static_cast<FLOAT>(logical.width);
        textMetrics->height = static_cast<FLOAT>(logical.height);
        textMetrics->layoutWidth = m_maxWidth;
        textMetrics->layoutHeight = m_maxHeight;
        textMetrics->lineCount = api.pango_layout_get_line_count
                                     ? static_cast<UINT32>(std::max(1, api.pango_layout_get_line_count(layout)))
                                     : 1u;
        return S_OK;
    }

    FLOAT lineH = m_fontSize * 1.28f;
    textMetrics->width = static_cast<FLOAT>(m_text.size()) * m_fontSize * 0.54f;
    if (m_maxWidth > 0.f && m_maxWidth < 1e6f && m_wordWrapping != DWRITE_WORD_WRAPPING_NO_WRAP) {
        textMetrics->width = std::min(textMetrics->width, m_maxWidth);
    }
    textMetrics->widthIncludingTrailingWhitespace = textMetrics->width;
    textMetrics->height = lineH;
    textMetrics->layoutWidth = m_maxWidth;
    textMetrics->layoutHeight = m_maxHeight;
    textMetrics->lineCount = 1;
    return S_OK;
}

HRESULT IDWriteTextLayout::HitTestPoint(FLOAT pointX, FLOAT pointY, BOOL* isTrailingHit,
                                        BOOL* isInside, DWRITE_HIT_TEST_METRICS* hitTestMetrics) {
    if (isTrailingHit) *isTrailingHit = FALSE;
    if (isInside) *isInside = FALSE;
    if (hitTestMetrics) std::memset(hitTestMetrics, 0, sizeof(*hitTestMetrics));

    auto& api = CairoPangoApi::Get();
    PangoLayout* layout = EnsurePangoLayout(this, GetScratchMetricCairo());
    if (layout && api.pango_layout_xy_to_index && api.pango_layout_index_to_pos) {
        int byteIdx = 0;
        int trailing = 0;
        int px = static_cast<int>(std::round(pointX * kPangoScale));
        int py = static_cast<int>(std::round(pointY * kPangoScale));
        int inside = api.pango_layout_xy_to_index(layout, px, py, &byteIdx, &trailing);
        if (isInside) *isInside = inside ? TRUE : FALSE;
        if (isTrailingHit) *isTrailingHit = (trailing > 0) ? TRUE : FALSE;

        if (hitTestMetrics) {
            PangoRectangle pos{};
            api.pango_layout_index_to_pos(layout, byteIdx, &pos);
            hitTestMetrics->textPosition = Utf8ByteOffsetToWideIndex(m_text, byteIdx);
            hitTestMetrics->length = 1;
            hitTestMetrics->left = static_cast<FLOAT>(pos.x) / kPangoScale;
            hitTestMetrics->top = static_cast<FLOAT>(pos.y) / kPangoScale;
            hitTestMetrics->width = static_cast<FLOAT>(std::abs(pos.width)) / kPangoScale;
            hitTestMetrics->height = static_cast<FLOAT>(pos.height) / kPangoScale;
            hitTestMetrics->isText = TRUE;
        }
        return S_OK;
    }

    if (hitTestMetrics) {
        FLOAT charW = std::max(1.0f, m_fontSize * 0.54f);
        UINT32 pos = pointX > 0.f ? static_cast<UINT32>(pointX / charW) : 0u;
        pos = std::min<UINT32>(pos, static_cast<UINT32>(m_text.size()));
        hitTestMetrics->textPosition = pos;
        hitTestMetrics->length = 1;
        hitTestMetrics->left = static_cast<FLOAT>(pos) * charW;
        hitTestMetrics->top = 0.0f;
        hitTestMetrics->width = charW;
        hitTestMetrics->height = m_fontSize * 1.28f;
        hitTestMetrics->isText = TRUE;
    }
    return S_OK;
}

HRESULT IDWriteTextLayout::HitTestTextPosition(UINT32 textPosition, BOOL isTrailingHit,
                                               FLOAT* pointX, FLOAT* pointY,
                                               DWRITE_HIT_TEST_METRICS* hitTestMetrics) {
    auto& api = CairoPangoApi::Get();
    PangoLayout* layout = EnsurePangoLayout(this, GetScratchMetricCairo());
    if (layout && api.pango_layout_index_to_pos) {
        UINT32 clamped = std::min<UINT32>(textPosition, static_cast<UINT32>(m_text.size()));
        int byteIdx = WideIndexToUtf8ByteOffset(m_text, clamped);
        PangoRectangle pos{};
        api.pango_layout_index_to_pos(layout, byteIdx, &pos);

        FLOAT left = static_cast<FLOAT>(pos.x) / kPangoScale;
        FLOAT top = static_cast<FLOAT>(pos.y) / kPangoScale;
        FLOAT width = static_cast<FLOAT>(std::abs(pos.width)) / kPangoScale;
        FLOAT height = static_cast<FLOAT>(pos.height) / kPangoScale;
        FLOAT xOut = isTrailingHit ? (left + width) : left;

        if (pointX) *pointX = xOut;
        if (pointY) *pointY = top;
        if (hitTestMetrics) {
            std::memset(hitTestMetrics, 0, sizeof(*hitTestMetrics));
            hitTestMetrics->textPosition = clamped;
            hitTestMetrics->length = (clamped < m_text.size()) ? 1u : 0u;
            hitTestMetrics->left = left;
            hitTestMetrics->top = top;
            hitTestMetrics->width = width;
            hitTestMetrics->height = height > 0.f ? height : m_fontSize * 1.28f;
            hitTestMetrics->isText = TRUE;
        }
        return S_OK;
    }

    FLOAT charW = std::max(1.0f, m_fontSize * 0.54f);
    FLOAT x = static_cast<FLOAT>(std::min<UINT32>(textPosition, static_cast<UINT32>(m_text.size()))) * charW;
    if (pointX) *pointX = x;
    if (pointY) *pointY = 0.0f;
    if (hitTestMetrics) {
        std::memset(hitTestMetrics, 0, sizeof(*hitTestMetrics));
        hitTestMetrics->textPosition = textPosition;
        hitTestMetrics->left = x;
        hitTestMetrics->width = charW;
        hitTestMetrics->height = m_fontSize * 1.28f;
        hitTestMetrics->isText = TRUE;
    }
    return S_OK;
}

// ============================================================================
// ID2D1Bitmap & ID2D1RenderTarget Lifecycle & Drawing
// ============================================================================

ID2D1Bitmap::~ID2D1Bitmap() {
    if (m_cachedCairoSurface) {
        auto& api = CairoPangoApi::Get();
        if (api.cairo_surface_destroy) {
            api.cairo_surface_destroy(static_cast<cairo_surface_t*>(m_cachedCairoSurface));
        }
        m_cachedCairoSurface = nullptr;
    }
}

ID2D1RenderTarget::ID2D1RenderTarget() = default;

ID2D1RenderTarget::~ID2D1RenderTarget() {
    if (m_cachedCr) {
        auto& api = CairoPangoApi::Get();
        if (api.cairo_destroy) {
            api.cairo_destroy(static_cast<cairo_t*>(m_cachedCr));
        }
        m_cachedCr = nullptr;
        m_cachedCrBitmap = nullptr;
    }
}

void ID2D1RenderTarget::BeginDraw() {
    m_pixelOriginX = 0;
    m_pixelOriginY = 0;
    m_layerStack.clear();
    m_clipStack.clear();
}

HRESULT ID2D1RenderTarget::EndDraw(UINT64* tag1, UINT64* tag2) {
    if (tag1) *tag1 = 0;
    if (tag2) *tag2 = 0;
    while (!m_layerStack.empty()) {
        PopLayer();
    }
    if (m_targetBitmap && m_targetBitmap->m_cachedCairoSurface) {
        auto& api = CairoPangoApi::Get();
        if (api.cairo_surface_flush) {
            api.cairo_surface_flush(static_cast<cairo_surface_t*>(m_targetBitmap->m_cachedCairoSurface));
        }
    }
    if (m_boundDC && m_targetBitmap && m_targetBitmap->HasPixels()) {
        LinuxBlitDCToBitmap(m_boundDC,
                            m_targetBitmap->PixelData(),
                            m_targetBitmap->m_width,
                            m_targetBitmap->m_height,
                            m_boundRect);
    }
    return S_OK;
}

void ID2D1RenderTarget::Clear(const D2D1_COLOR_F& clearColor) {
    if (!m_targetBitmap || !m_targetBitmap->HasPixels()) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        if (api.cairo_identity_matrix) api.cairo_identity_matrix(cr);
        if (api.cairo_set_operator) api.cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        api.cairo_set_source_rgba(cr, clearColor.r, clearColor.g, clearColor.b, clearColor.a);
        api.cairo_paint(cr);
        api.cairo_restore(cr);
        return;
    }
    uint8_t a = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, clearColor.a)) * 255.0f);
    uint8_t r = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, clearColor.r * clearColor.a)) * 255.0f);
    uint8_t g = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, clearColor.g * clearColor.a)) * 255.0f);
    uint8_t b = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, clearColor.b * clearColor.a)) * 255.0f);
    uint32_t px = (static_cast<uint32_t>(a) << 24) |
                  (static_cast<uint32_t>(r) << 16) |
                  (static_cast<uint32_t>(g) << 8) |
                  static_cast<uint32_t>(b);
    uint32_t* p = reinterpret_cast<uint32_t*>(m_targetBitmap->PixelData());
    size_t count = m_targetBitmap->PixelByteCount() / 4;
    std::fill_n(p, count, px);
}

void ID2D1RenderTarget::DrawLine(D2D1_POINT_2F point0, D2D1_POINT_2F point1, ID2D1Brush* brush,
                                 FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle) {
    if (!brush) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        ApplyStrokeStyle(cr, strokeStyle);
        api.cairo_set_line_width(cr, strokeWidth);
        api.cairo_new_path(cr);
        api.cairo_move_to(cr, point0.x, point0.y);
        api.cairo_line_to(cr, point1.x, point1.y);
        api.cairo_stroke(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::DrawRectangle(const D2D1_RECT_F& rect, ID2D1Brush* brush,
                                      FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle) {
    if (!brush) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        ApplyStrokeStyle(cr, strokeStyle);
        api.cairo_set_line_width(cr, strokeWidth);
        api.cairo_new_path(cr);
        api.cairo_rectangle(cr, rect.left, rect.top,
                            std::max(0.0f, rect.right - rect.left),
                            std::max(0.0f, rect.bottom - rect.top));
        api.cairo_stroke(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::FillRectangle(const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    if (!brush) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        api.cairo_new_path(cr);
        api.cairo_rectangle(cr, rect.left, rect.top,
                            std::max(0.0f, rect.right - rect.left),
                            std::max(0.0f, rect.bottom - rect.top));
        api.cairo_fill(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::DrawRoundedRectangle(const D2D1_ROUNDED_RECT& roundedRect, ID2D1Brush* brush,
                                             FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle) {
    if (!brush) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        ApplyStrokeStyle(cr, strokeStyle);
        api.cairo_set_line_width(cr, strokeWidth);
        EmitRoundedRectPath(cr, roundedRect);
        api.cairo_stroke(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::FillRoundedRectangle(const D2D1_ROUNDED_RECT& roundedRect, ID2D1Brush* brush) {
    if (!brush) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        EmitRoundedRectPath(cr, roundedRect);
        api.cairo_fill(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::DrawEllipse(const D2D1_ELLIPSE& ellipse, ID2D1Brush* brush,
                                    FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle) {
    if (!brush || ellipse.radiusX <= 0.f || ellipse.radiusY <= 0.f) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        ApplyStrokeStyle(cr, strokeStyle);
        api.cairo_set_line_width(cr, strokeWidth);
        api.cairo_save(cr);
        api.cairo_translate(cr, ellipse.point.x, ellipse.point.y);
        api.cairo_scale(cr, ellipse.radiusX, ellipse.radiusY);
        api.cairo_new_path(cr);
        api.cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * kPi);
        api.cairo_restore(cr);
        api.cairo_stroke(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::FillEllipse(const D2D1_ELLIPSE& ellipse, ID2D1Brush* brush) {
    if (!brush || ellipse.radiusX <= 0.f || ellipse.radiusY <= 0.f) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        api.cairo_translate(cr, ellipse.point.x, ellipse.point.y);
        api.cairo_scale(cr, ellipse.radiusX, ellipse.radiusY);
        api.cairo_new_path(cr);
        api.cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * kPi);
        api.cairo_fill(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::DrawGeometry(ID2D1Geometry* geometry, ID2D1Brush* brush,
                                     FLOAT strokeWidth, ID2D1StrokeStyle* strokeStyle) {
    if (!geometry || !brush) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        ApplyStrokeStyle(cr, strokeStyle);
        api.cairo_set_line_width(cr, strokeWidth);
        EmitGeometryToCairo(cr, geometry);
        api.cairo_stroke(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::FillGeometry(ID2D1Geometry* geometry, ID2D1Brush* brush, ID2D1Brush* /*opacityBrush*/) {
    if (!geometry || !brush) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, brush);
        EmitGeometryToCairo(cr, geometry);
        api.cairo_fill(cr);
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::DrawTextLayout(D2D1_POINT_2F origin, IDWriteTextLayout* textLayout,
                                       ID2D1Brush* defaultFillBrush, D2D1_DRAW_TEXT_OPTIONS options) {
    if (!textLayout || !defaultFillBrush || textLayout->m_text.empty()) return;
    if (cairo_t* cr = EnsureTargetCairo(this)) {
        auto& api = CairoPangoApi::Get();
        if (!api.hasPango()) return;
        api.cairo_save(cr);
        ApplyRTState(cr, this);
        ApplyBrushToCairo(cr, defaultFillBrush);
        if ((options & D2D1_DRAW_TEXT_OPTIONS_CLIP) != 0 &&
            HasBoundedLayoutWidth(textLayout) &&
            textLayout->m_maxHeight > 0.0f && textLayout->m_maxHeight < 1e6f) {
            api.cairo_rectangle(cr, origin.x, origin.y, textLayout->m_maxWidth, textLayout->m_maxHeight);
            api.cairo_clip(cr);
        }

        PangoLayout* layout = EnsurePangoLayout(textLayout, cr);
        if (layout) {
            PangoRectangle logical{};
            api.pango_layout_get_pixel_extents(layout, nullptr, &logical);
            const double xOffset = ComputeUnboundedHorizontalOffset(textLayout, logical.width);
            double yOffset = 0.0;
            if (textLayout->m_paragraphAlignment != DWRITE_PARAGRAPH_ALIGNMENT_NEAR &&
                textLayout->m_maxHeight > 0.0f && textLayout->m_maxHeight < 1e6f) {
                if (textLayout->m_paragraphAlignment == DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
                    yOffset = (textLayout->m_maxHeight - logical.height) * 0.5 - logical.y;
                } else if (textLayout->m_paragraphAlignment == DWRITE_PARAGRAPH_ALIGNMENT_FAR) {
                    yOffset = textLayout->m_maxHeight - logical.height - logical.y;
                }
            }
            api.cairo_move_to(cr, origin.x + xOffset, origin.y + yOffset);
            api.pango_cairo_show_layout(cr, layout);
        }
        api.cairo_restore(cr);
    }
}

void ID2D1RenderTarget::DrawText(const WCHAR* string, UINT32 stringLength, IDWriteTextFormat* textFormat,
                                 const D2D1_RECT_F& layoutRect, ID2D1Brush* defaultFillBrush,
                                 D2D1_DRAW_TEXT_OPTIONS options, DWRITE_MEASURING_MODE /*measuringMode*/) {
    if (!string || stringLength == 0 || !defaultFillBrush) return;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> tl(new IDWriteTextLayout());
    if (textFormat) {
        tl->m_familyName = textFormat->m_familyName;
        tl->m_weight = textFormat->m_weight;
        tl->m_style = textFormat->m_style;
        tl->m_stretch = textFormat->m_stretch;
        tl->m_fontSize = textFormat->m_fontSize;
        tl->m_textAlignment = textFormat->m_textAlignment;
        tl->m_paragraphAlignment = textFormat->m_paragraphAlignment;
        tl->m_wordWrapping = textFormat->m_wordWrapping;
        tl->m_trimming = textFormat->m_trimming;
    }
    tl->m_text.assign(string, stringLength);
    tl->m_maxWidth = std::max(1.0f, layoutRect.right - layoutRect.left);
    tl->m_maxHeight = std::max(1.0f, layoutRect.bottom - layoutRect.top);
    DrawTextLayout(D2D1::Point2F(layoutRect.left, layoutRect.top), tl.Get(), defaultFillBrush, options);
}

void ID2D1RenderTarget::DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destinationRectangle, FLOAT opacity,
                                   D2D1_BITMAP_INTERPOLATION_MODE interpolationMode, const D2D1_RECT_F* sourceRectangle) {
    if (!bitmap || !bitmap->HasPixels() || bitmap->m_width == 0 || bitmap->m_height == 0 || opacity <= 0.0f) {
        return;
    }
    cairo_t* cr = EnsureTargetCairo(this);
    cairo_surface_t* srcSurf = EnsureCairoSurfaceForBitmap(bitmap);
    if (!cr || !srcSurf) return;

    auto& api = CairoPangoApi::Get();
    if (api.cairo_surface_flush) api.cairo_surface_flush(srcSurf);

    D2D1_SIZE_F bmpDip = bitmap->GetSize();
    const D2D1_RECT_F& dst = destinationRectangle;
    D2D1_RECT_F srcDip = sourceRectangle
                             ? *sourceRectangle
                             : D2D1::RectF(0.0f, 0.0f, bmpDip.width, bmpDip.height);

    double dstW = dst.right - dst.left;
    double dstH = dst.bottom - dst.top;
    if (dstW <= 0.0 || dstH <= 0.0) return;

    double bmpDpiX = bitmap->m_dpiX > 0.0f ? bitmap->m_dpiX : 96.0;
    double bmpDpiY = bitmap->m_dpiY > 0.0f ? bitmap->m_dpiY : 96.0;
    double sxPx = srcDip.left * (bmpDpiX / 96.0);
    double syPx = srcDip.top * (bmpDpiY / 96.0);
    double swPx = (srcDip.right - srcDip.left) * (bmpDpiX / 96.0);
    double shPx = (srcDip.bottom - srcDip.top) * (bmpDpiY / 96.0);
    if (swPx <= 0.0 || shPx <= 0.0) return;

    api.cairo_save(cr);
    ApplyRTState(cr, this);

    api.cairo_rectangle(cr, dst.left, dst.top, dstW, dstH);
    api.cairo_clip(cr);

    if (api.cairo_pattern_create_for_surface && api.cairo_pattern_set_matrix &&
        api.cairo_matrix_init && api.cairo_set_source && api.cairo_pattern_destroy) {
        cairo_pattern_t* pat = api.cairo_pattern_create_for_surface(srcSurf);
        if (pat) {
            if (api.cairo_pattern_set_extend) api.cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
            if (api.cairo_pattern_set_filter) {
                api.cairo_pattern_set_filter(
                    pat,
                    interpolationMode == D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                        ? CAIRO_FILTER_NEAREST
                        : CAIRO_FILTER_GOOD);
            }
            // Maps user (DIP) -> pattern (source pixel) space.
            double scaleX = swPx / dstW;
            double scaleY = shPx / dstH;
            cairo_matrix_t mat{};
            api.cairo_matrix_init(&mat,
                                  scaleX, 0.0,
                                  0.0, scaleY,
                                  sxPx - dst.left * scaleX,
                                  syPx - dst.top * scaleY);
            api.cairo_pattern_set_matrix(pat, &mat);
            api.cairo_set_source(cr, pat);
            api.cairo_rectangle(cr, dst.left, dst.top, dstW, dstH);
            if (opacity >= 0.999f) {
                api.cairo_fill(cr);
            } else {
                api.cairo_clip(cr);
                api.cairo_paint_with_alpha(cr, opacity);
            }
            api.cairo_pattern_destroy(pat);
            api.cairo_restore(cr);
            return;
        }
    }

    api.cairo_translate(cr, dst.left, dst.top);
    api.cairo_scale(cr, dstW / swPx, dstH / shPx);
    api.cairo_set_source_surface(cr, srcSurf, -sxPx, -syPx);
    if (opacity >= 0.999f) api.cairo_paint(cr);
    else api.cairo_paint_with_alpha(cr, opacity);
    api.cairo_restore(cr);
}

void ID2D1RenderTarget::PushLayer(const D2D1_LAYER_PARAMETERS& layerParameters, ID2D1Layer* /*layer*/) {
    LinuxLayerState state;
    state.savedTarget = m_targetBitmap;
    state.savedOriginX = m_pixelOriginX;
    state.savedOriginY = m_pixelOriginY;
    state.opacity = layerParameters.opacity;
    state.geometricMask = layerParameters.geometricMask;
    state.maskTransform = layerParameters.maskTransform;
    state.savedClipStack = m_clipStack;

    if (!m_targetBitmap || m_targetBitmap->m_width == 0 || m_targetBitmap->m_height == 0) {
        m_layerStack.push_back(std::move(state));
        return;
    }

    double sx = 1.0, sy = 1.0;
    GetDpiScale(this, sx, sy);

    int targetW = static_cast<int>(m_targetBitmap->m_width);
    int targetH = static_cast<int>(m_targetBitmap->m_height);
    int minPxX = 0;
    int minPxY = 0;
    int maxPxX = targetW;
    int maxPxY = targetH;

    const D2D1_RECT_F& cb = layerParameters.contentBounds;
    if (cb.right > cb.left && cb.bottom > cb.top &&
        (cb.right - cb.left) < 16384.0f && (cb.bottom - cb.top) < 16384.0f) {
        D2D1_POINT_2F corners[4] = {
            TransformPt(m_transform, cb.left, cb.top),
            TransformPt(m_transform, cb.right, cb.top),
            TransformPt(m_transform, cb.right, cb.bottom),
            TransformPt(m_transform, cb.left, cb.bottom),
        };
        double x0 = corners[0].x, x1 = corners[0].x;
        double y0 = corners[0].y, y1 = corners[0].y;
        for (int i = 1; i < 4; ++i) {
            x0 = std::min(x0, static_cast<double>(corners[i].x));
            x1 = std::max(x1, static_cast<double>(corners[i].x));
            y0 = std::min(y0, static_cast<double>(corners[i].y));
            y1 = std::max(y1, static_cast<double>(corners[i].y));
        }
        int bx0 = static_cast<int>(std::floor(x0 * sx)) - m_pixelOriginX;
        int by0 = static_cast<int>(std::floor(y0 * sy)) - m_pixelOriginY;
        int bx1 = static_cast<int>(std::ceil(x1 * sx)) - m_pixelOriginX;
        int by1 = static_cast<int>(std::ceil(y1 * sy)) - m_pixelOriginY;
        minPxX = std::max(0, bx0);
        minPxY = std::max(0, by0);
        maxPxX = std::min(targetW, bx1);
        maxPxY = std::min(targetH, by1);
    }

    if (maxPxX <= minPxX || maxPxY <= minPxY) {
        m_targetBitmap = nullptr;
        m_layerStack.push_back(std::move(state));
        return;
    }

    UINT32 lw = static_cast<UINT32>(maxPxX - minPxX);
    UINT32 lh = static_cast<UINT32>(maxPxY - minPxY);
    state.originX = m_pixelOriginX + minPxX;
    state.originY = m_pixelOriginY + minPxY;
    state.layerBitmap = AcquireLayerBitmap(this, lw, lh);

    m_targetBitmap = state.layerBitmap;
    m_pixelOriginX = state.originX;
    m_pixelOriginY = state.originY;
    m_layerStack.push_back(std::move(state));
}

void ID2D1RenderTarget::PopLayer() {
    if (m_layerStack.empty()) return;
    LinuxLayerState state = std::move(m_layerStack.back());
    m_layerStack.pop_back();

    auto& api = CairoPangoApi::Get();
    if (state.layerBitmap && state.layerBitmap->m_cachedCairoSurface && api.cairo_surface_flush) {
        api.cairo_surface_flush(static_cast<cairo_surface_t*>(state.layerBitmap->m_cachedCairoSurface));
    }

    m_targetBitmap = state.savedTarget;
    m_pixelOriginX = state.savedOriginX;
    m_pixelOriginY = state.savedOriginY;
    m_clipStack = std::move(state.savedClipStack);

    if (state.layerBitmap && m_targetBitmap && state.opacity > 0.0f) {
        cairo_t* cr = EnsureTargetCairo(this);
        cairo_surface_t* layerSurf = EnsureCairoSurfaceForBitmap(state.layerBitmap.Get());
        if (cr && layerSurf) {
            api.cairo_save(cr);
            ApplyRTState(cr, this);

            if (state.geometricMask) {
                if (api.cairo_transform) {
                    cairo_matrix_t maskMat{
                        state.maskTransform.m11, state.maskTransform.m12,
                        state.maskTransform.m21, state.maskTransform.m22,
                        state.maskTransform.dx,  state.maskTransform.dy
                    };
                    api.cairo_transform(cr, &maskMat);
                }
                EmitGeometryToCairo(cr, state.geometricMask.Get());
                api.cairo_clip(cr);
            }

            if (api.cairo_identity_matrix) api.cairo_identity_matrix(cr);
            if (m_pixelOriginX != 0 || m_pixelOriginY != 0) {
                api.cairo_translate(cr, -static_cast<double>(m_pixelOriginX), -static_cast<double>(m_pixelOriginY));
            }
            api.cairo_set_source_surface(
                cr, layerSurf,
                static_cast<double>(state.originX),
                static_cast<double>(state.originY));
            if (state.opacity >= 0.999f) api.cairo_paint(cr);
            else api.cairo_paint_with_alpha(cr, state.opacity);
            api.cairo_restore(cr);
        }
    }

    RecycleLayerBitmap(this, std::move(state.layerBitmap));
}

HRESULT ID2D1RenderTarget::CreateBitmapFromWicBitmap(
    IWICBitmapSource* wicBitmapSource,
    const D2D1_BITMAP_PROPERTIES* bitmapProperties,
    ID2D1Bitmap** bitmap) {
    if (!bitmap) return E_POINTER;
    if (!wicBitmapSource) return E_INVALIDARG;
    UINT w = 0, h = 0;
    if (FAILED(wicBitmapSource->GetSize(&w, &h)) || w == 0 || h == 0 || wicBitmapSource->m_pixels.empty()) {
        return E_FAIL;
    }

    auto* bmp = new ID2D1Bitmap1(w, h);
    if (bitmapProperties) {
        if (bitmapProperties->dpiX > 0.0f) bmp->m_dpiX = bitmapProperties->dpiX;
        if (bitmapProperties->dpiY > 0.0f) bmp->m_dpiY = bitmapProperties->dpiY;
    }

    uint8_t* dst = bmp->PixelData();
    const uint8_t* src = wicBitmapSource->m_pixels.data();
    size_t count = static_cast<size_t>(w) * h;
    if (wicBitmapSource->m_isPremultiplied) {
        std::memcpy(dst, src, count * 4);
    } else {
        for (size_t i = 0; i < count; ++i) {
            uint8_t a = src[i * 4 + 3];
            if (a == 255) {
                std::memcpy(dst + i * 4, src + i * 4, 4);
            } else if (a == 0) {
                std::memset(dst + i * 4, 0, 4);
            } else {
                dst[i * 4 + 0] = static_cast<uint8_t>((static_cast<unsigned>(src[i * 4 + 0]) * a + 127) / 255);
                dst[i * 4 + 1] = static_cast<uint8_t>((static_cast<unsigned>(src[i * 4 + 1]) * a + 127) / 255);
                dst[i * 4 + 2] = static_cast<uint8_t>((static_cast<unsigned>(src[i * 4 + 2]) * a + 127) / 255);
                dst[i * 4 + 3] = a;
            }
        }
    }
    bmp->m_dirtyForCairo = true;
    *bitmap = bmp;
    return S_OK;
}

HRESULT ID2D1RenderTarget::CreateCompatibleRenderTarget(
    D2D1_SIZE_F desiredSize,
    ID2D1BitmapRenderTarget** bitmapRenderTarget) {
    if (!bitmapRenderTarget) return E_POINTER;
    FLOAT sx = m_dpiX > 0.0f ? (m_dpiX / 96.0f) : 1.0f;
    FLOAT sy = m_dpiY > 0.0f ? (m_dpiY / 96.0f) : 1.0f;
    UINT32 pw = static_cast<UINT32>(std::max(1.0f, std::ceil(desiredSize.width * sx)));
    UINT32 ph = static_cast<UINT32>(std::max(1.0f, std::ceil(desiredSize.height * sy)));
    auto* rt = new ID2D1BitmapRenderTarget(pw, ph);
    rt->SetDpi(m_dpiX, m_dpiY);
    *bitmapRenderTarget = rt;
    return S_OK;
}

void ID2D1RenderTarget::PushAxisAlignedClip(const D2D1_RECT_F& clipRect, D2D1_ANTIALIAS_MODE /*antialiasMode*/) {
    m_clipStack.push_back(clipRect);
}

void ID2D1RenderTarget::PopAxisAlignedClip() {
    if (!m_clipStack.empty()) {
        m_clipStack.pop_back();
    }
}

void ID2D1RenderTarget::PushLayer(const D2D1_LAYER_PARAMETERS1& layerParameters, ID2D1Layer* layer) {
    PushLayer(D2D1::LayerParameters(
                  layerParameters.contentBounds,
                  layerParameters.geometricMask,
                  layerParameters.maskAntialiasMode,
                  layerParameters.maskTransform,
                  layerParameters.opacity,
                  layerParameters.opacityBrush,
                  static_cast<D2D1_LAYER_OPTIONS>(layerParameters.layerOptions)),
              layer);
}

HRESULT ID2D1DCRenderTarget::BindDC(const HDC hDC, const RECT* pSubRect) {
    m_boundDC = hDC;
    if (pSubRect) m_boundRect = *pSubRect;
    UINT32 w = static_cast<UINT32>(std::max<LONG>(1, m_boundRect.right - m_boundRect.left));
    UINT32 h = static_cast<UINT32>(std::max<LONG>(1, m_boundRect.bottom - m_boundRect.top));
    if (!m_targetBitmap || m_targetBitmap->m_width != w || m_targetBitmap->m_height != h) {
        m_targetBitmap = new ID2D1Bitmap1(w, h);
    } else if (m_targetBitmap->HasPixels()) {
        std::memset(m_targetBitmap->PixelData(), 0, m_targetBitmap->PixelByteCount());
        m_targetBitmap->m_dirtyForCairo = true;
    }
    return S_OK;
}

// ============================================================================
// WIC Decoding, Scaling & Encoding
// ============================================================================

HRESULT IWICStream::InitializeFromFilename(LPCWSTR wzFileName, DWORD /*dwDesiredAccess*/) {
    if (!wzFileName) return E_INVALIDARG;
    m_data = ReadFileBytes(wzFileName);
    m_pos = 0;
    return m_data.empty() ? E_FAIL : S_OK;
}

HRESULT IWICBitmapScaler::Initialize(IWICBitmapSource* pISource, UINT uiWidth, UINT uiHeight,
                                     WICBitmapInterpolationMode /*mode*/) {
    if (!pISource || uiWidth == 0 || uiHeight == 0) return E_INVALIDARG;
    UINT sw = 0, sh = 0;
    if (FAILED(pISource->GetSize(&sw, &sh)) || sw == 0 || sh == 0 || pISource->m_pixels.empty()) {
        return E_FAIL;
    }
    m_width = uiWidth;
    m_height = uiHeight;
    m_isPremultiplied = pISource->m_isPremultiplied;
    m_pixels.resize(static_cast<size_t>(uiWidth) * uiHeight * 4);
    for (UINT y = 0; y < uiHeight; ++y) {
        double fy = (static_cast<double>(y) + 0.5) * sh / uiHeight - 0.5;
        UINT y0 = static_cast<UINT>(std::max(0.0, std::min<double>(sh - 1, std::floor(fy))));
        UINT y1 = std::min(y0 + 1, sh - 1);
        double wy = std::max(0.0, std::min(1.0, fy - y0));
        for (UINT x = 0; x < uiWidth; ++x) {
            double fx = (static_cast<double>(x) + 0.5) * sw / uiWidth - 0.5;
            UINT x0 = static_cast<UINT>(std::max(0.0, std::min<double>(sw - 1, std::floor(fx))));
            UINT x1 = std::min(x0 + 1, sw - 1);
            double wx = std::max(0.0, std::min(1.0, fx - x0));

            const uint8_t* p00 = &pISource->m_pixels[(static_cast<size_t>(y0) * sw + x0) * 4];
            const uint8_t* p10 = &pISource->m_pixels[(static_cast<size_t>(y0) * sw + x1) * 4];
            const uint8_t* p01 = &pISource->m_pixels[(static_cast<size_t>(y1) * sw + x0) * 4];
            const uint8_t* p11 = &pISource->m_pixels[(static_cast<size_t>(y1) * sw + x1) * 4];
            uint8_t* dst = &m_pixels[(static_cast<size_t>(y) * uiWidth + x) * 4];
            for (int c = 0; c < 4; ++c) {
                double v = (p00[c] * (1.0 - wx) + p10[c] * wx) * (1.0 - wy) +
                           (p01[c] * (1.0 - wx) + p11[c] * wx) * wy;
                dst[c] = static_cast<uint8_t>(std::max(0.0, std::min(255.0, v + 0.5)));
            }
        }
    }
    return S_OK;
}

HRESULT IWICBitmapDecoder::Initialize(IUnknown* pIStream, WICDecodeOptions /*cacheOptions*/) {
    std::vector<uint8_t> bytes = ReadStreamBytes(pIStream);
    if (bytes.empty()) return E_FAIL;

    auto* frame = new IWICBitmapFrameDecode();
    if (!DecodeImageBytesToBGRA(bytes.data(), bytes.size(),
                                frame->m_pixels, frame->m_width, frame->m_height,
                                frame->m_isPremultiplied)) {
        delete frame;
        return E_FAIL;
    }
    m_frame = frame;
    return S_OK;
}

HRESULT IWICImagingFactory::CreateDecoderFromStream(IUnknown* pIStream, const GUID* /*pguidVendor*/,
                                                    WICDecodeOptions metadataOptions,
                                                    IWICBitmapDecoder** ppIDecoder) {
    if (!ppIDecoder) return E_POINTER;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> dec(new IWICBitmapDecoder());
    HRESULT hr = dec->Initialize(pIStream, metadataOptions);
    if (FAILED(hr)) return hr;
    *ppIDecoder = dec.Detach();
    return S_OK;
}

HRESULT IWICImagingFactory::CreateDecoderFromFilename(LPCWSTR wzFilename, const GUID* /*pguidVendor*/,
                                                      DWORD /*dwDesiredAccess*/, WICDecodeOptions /*metadataOptions*/,
                                                      IWICBitmapDecoder** ppIDecoder) {
    if (!wzFilename || !ppIDecoder) return E_POINTER;
    std::vector<uint8_t> bytes = ReadFileBytes(wzFilename);
    if (bytes.empty()) return E_FAIL;

    auto* frame = new IWICBitmapFrameDecode();
    if (!DecodeImageBytesToBGRA(bytes.data(), bytes.size(),
                                frame->m_pixels, frame->m_width, frame->m_height,
                                frame->m_isPremultiplied)) {
        delete frame;
        return E_FAIL;
    }
    auto* dec = new IWICBitmapDecoder();
    dec->m_frame = frame;
    *ppIDecoder = dec;
    return S_OK;
}

HRESULT IWICBitmapFrameEncode::WritePixels(UINT lineCount, UINT cbStride, UINT cbBufferSize, BYTE* pbPixels) {
    if (!m_stream || !pbPixels || lineCount == 0 || cbStride == 0) return E_INVALIDARG;
    const bool srcIsBGR24 = (m_format == GUID_WICPixelFormat24bppBGR);
    const UINT srcBpp = srcIsBGR24 ? 3u : 4u;
    UINT w = m_width;
    UINT h = std::min(lineCount, m_height > 0 ? m_height : lineCount);
    if (w == 0 || h == 0 || cbStride < static_cast<size_t>(w) * srcBpp) return E_INVALIDARG;

    if (static_cast<size_t>(h - 1) * cbStride + static_cast<size_t>(w) * srcBpp > cbBufferSize) {
        if (cbBufferSize < static_cast<size_t>(w) * srcBpp) return E_INVALIDARG;
        h = std::min<UINT>(h, static_cast<UINT>((cbBufferSize - static_cast<size_t>(w) * srcBpp) / cbStride + 1));
        if (h == 0) return E_INVALIDARG;
    }

    if (m_containerFormat == GUID_ContainerFormatJpeg) {
        auto& api = GdkPixbufApi::Get();
        if (!api.loaded || !api.gdk_pixbuf_new_from_data || !api.gdk_pixbuf_save_to_buffer || !api.g_free) {
            return E_FAIL;
        }
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        for (UINT y = 0; y < h; ++y) {
            const uint8_t* srow = pbPixels + static_cast<size_t>(y) * cbStride;
            uint8_t* drow = rgb.data() + static_cast<size_t>(y) * w * 3;
            for (UINT x = 0; x < w; ++x) {
                drow[x * 3 + 0] = srow[x * srcBpp + 2];
                drow[x * 3 + 1] = srow[x * srcBpp + 1];
                drow[x * 3 + 2] = srow[x * srcBpp + 0];
            }
        }
        GdkPixbuf* pb = api.gdk_pixbuf_new_from_data(
            rgb.data(), 0 /*GDK_COLORSPACE_RGB*/, 0, 8,
            static_cast<int>(w), static_cast<int>(h), static_cast<int>(w * 3),
            nullptr, nullptr);
        if (!pb) return E_FAIL;

        int q = std::max(1, std::min(100, static_cast<int>(std::round(m_jpegQuality * 100.0f))));
        char qbuf[16];
        std::snprintf(qbuf, sizeof(qbuf), "%d", q);

        char* outBuf = nullptr;
        size_t outLen = 0;
        GError* err = nullptr;
        int ok = api.gdk_pixbuf_save_to_buffer(pb, &outBuf, &outLen, "jpeg", &err, "quality", qbuf, nullptr);
        api.g_object_unref(pb);
        if (!ok || !outBuf || outLen == 0) {
            if (err && api.g_error_free) api.g_error_free(err);
            if (outBuf) api.g_free(outBuf);
            return E_FAIL;
        }
        ULONG written = 0;
        HRESULT hr = m_stream->Write(outBuf, static_cast<ULONG>(outLen), &written);
        api.g_free(outBuf);
        return hr;
    }

    auto& api = CairoPangoApi::Get();
    if (!api.loaded || !api.cairo_image_surface_create_for_data || !api.cairo_surface_write_to_png_stream) {
        return E_FAIL;
    }

    std::vector<uint8_t> argb(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* srow = pbPixels + static_cast<size_t>(y) * cbStride;
        uint8_t* drow = argb.data() + static_cast<size_t>(y) * w * 4;
        if (srcIsBGR24) {
            for (UINT x = 0; x < w; ++x) {
                drow[x * 4 + 0] = srow[x * 3 + 0];
                drow[x * 4 + 1] = srow[x * 3 + 1];
                drow[x * 4 + 2] = srow[x * 3 + 2];
                drow[x * 4 + 3] = 255;
            }
        } else {
            for (UINT x = 0; x < w; ++x) {
                uint8_t b = srow[x * 4 + 0];
                uint8_t g = srow[x * 4 + 1];
                uint8_t r = srow[x * 4 + 2];
                uint8_t a = srow[x * 4 + 3];
                if (a < 255) {
                    b = static_cast<uint8_t>((static_cast<unsigned>(b) * a + 127) / 255);
                    g = static_cast<uint8_t>((static_cast<unsigned>(g) * a + 127) / 255);
                    r = static_cast<uint8_t>((static_cast<unsigned>(r) * a + 127) / 255);
                }
                drow[x * 4 + 0] = b;
                drow[x * 4 + 1] = g;
                drow[x * 4 + 2] = r;
                drow[x * 4 + 3] = a;
            }
        }
    }

    cairo_surface_t* surf = api.cairo_image_surface_create_for_data(
        argb.data(), CAIRO_FORMAT_ARGB32, static_cast<int>(w), static_cast<int>(h), static_cast<int>(w * 4));
    if (!surf) return E_FAIL;

    std::vector<uint8_t> encoded;
    cairo_status_t st = api.cairo_surface_write_to_png_stream(surf, CairoVecWrite, &encoded);
    api.cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS || encoded.empty()) return E_FAIL;

    ULONG written = 0;
    return m_stream->Write(encoded.data(), static_cast<ULONG>(encoded.size()), &written);
}
