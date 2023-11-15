layout_t DEFAULT = "code";
layout_t ENGLISH = "us";
layout_t SWEDISH = "se";
layout_t TEXT = "text";
layout_t CHESS = "chess";

static layout_t select_layout(const struct state* st, const struct window* w)
{
    if(window_has_class(w, "st-256color")
       || window_has_class(w, "chromium")
       ) {
        return DEFAULT;
    }

    if(window_has_class(w, "musescore")
       || window_has_class(w, "BaldursGate")
       || window_has_class(w, "Dwarf_Fortress")
       || window_has_class(w, "nethack")
       || window_has_class(w, "Stardew Valley")
       || window_has_name(w, "Caesar III")
       || window_has_class(w, "devilutionx")
       || window_has_class(w, "adom64")
       || window_has_class(w, "ecwolf")
       || window_has_class(w, "FTL.amd64")
       || window_has_class(w, "Breach")
       || window_has_class(w, "Chowdren")
       || window_has_class(w, "cogmind.exe")
       ) {
        return ENGLISH;
    }

    if(window_has_class_rec(st, w, "scid")) {
        return CHESS;
    }
    if(window_has_class_rec(st, w, "setup")) {
        return ENGLISH;
    }

    if(window_has_class(w, "adom")) {
        return "adom";
    }

    return DEFAULT;
}
