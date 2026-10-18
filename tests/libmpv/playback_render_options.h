// Capture effective options explicitly; player defaults are not a quality match.
static bool ReportPlaybackRenderOptions(mpv_handle* player) {
    const char* names[] = {"scale", "dscale", "cscale", "linear-downscaling",
        "correct-downscaling", "sigmoid-upscaling", "dither-depth", "dither", "target-colorspace-hint"};
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
    printf("}\n");
    return complete;
}
