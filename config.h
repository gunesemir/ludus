/* Grid dimensions in Wayland surface coordinates (covers are 2:3). */
#define COVER_WIDTH       216
#define SPACING            16

/* Fontconfig family and pixel sizes. */
#define FONT_PATTERN      "monospace"
#define HEADER_FONT_SIZE   32
#define STATUS_FONT_SIZE   16
#define FONT_SIZE          16  /* Game titles and form fields */

/* Colors in 0xRRGGBB. */
#define BACKGROUND        0x000000
#define BAR_BACKGROUND    0x222222
#define BORDER_COLOR      0xffffff

/* Library keybindings (XKB keysyms). */
#define KEY_LEFT          XKB_KEY_h
#define KEY_DOWN          XKB_KEY_j
#define KEY_UP            XKB_KEY_k
#define KEY_RIGHT         XKB_KEY_l
#define KEY_LAUNCH        XKB_KEY_Return
#define KEY_EDIT          XKB_KEY_e
#define KEY_QUIT          XKB_KEY_q
#define KEY_SEARCH        XKB_KEY_slash
#define KEY_NEXT_MATCH    XKB_KEY_n
#define KEY_PREV_MATCH    XKB_KEY_N
