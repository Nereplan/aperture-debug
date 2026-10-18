// Capture effective options explicitly; player defaults are not a quality match.
static bool ReportPlaybackRenderOptions(mpv_handle* player) {
    const char* names[] = {"scale", "dscale", "cscale", "linear-downscaling",
        "correct-downscaling", "sigmoid-upscaling", "dither-depth", "dither", "target-colorspace-hint", "fbo-format"};
    bool complete = true;
    printf("RENDER_OPTIONS {");
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char* text = fn_get_property_string(player, names[i]);
        complete = complete && text != nullptr;
        const std::string value = text ? text : "";
        if (text) fn_free(text);
        if (i) printf(",");
        printf("\"%s\":\"", names[i]);
        for (unsigned char c : value) {
            if (c == '\\' || c == '"') printf("\\%c", c);
            else if (c < 32) printf("\\u%04x", c);
            else putchar(c);
        }
        printf("\"");
    }
    // Older baseline DLLs do not expose Aperture's frame-resource options.
    for (const char* name : {"d3d11-gpu-timing", "d3d11-vertex-cache-entries", "fbo-sdr10", "scale-direct-nv12",
                            "video-out-params/pixelformat", "video-params/primaries", "video-params/gamma", "video-params/alpha"}) {
        char* value = fn_get_property_string(player, name);
        if (value) {
            // These are constrained boolean/integer/format identifiers.
            printf(",\"%s\":\"%s\"", name, value);
            fn_free(value);
        }
    }
    printf("}\n");
    return complete;
}
