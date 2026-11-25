/* Grid dimensions in Wayland surface coordinates (covers are 2:3). */
#define COVER_WIDTH       216  /* 2..4096 */
#define SPACING            16  /* 4..1024 */

/* Fontconfig family and pixel sizes. */
#define FONT_PATTERN      "monospace"
#define HEADER_FONT_SIZE   32
#define STATUS_FONT_SIZE   16
#define TITLE_FONT_SIZE    16

/* Colors in 0xRRGGBB. */
#define BACKGROUND        0x000000
#define BAR_BACKGROUND    0x222222
#define TEXT_COLOR        0xffffff
#define ACCENT_COLOR      0xffffff  /* Selected cover outline and prompt caret */
#define EMPTY_COVER_COLOR 0x38393b

/* Library keybindings (XKB keysyms). */
#define KEY_LEFT          XKB_KEY_h
#define KEY_DOWN          XKB_KEY_j
#define KEY_UP            XKB_KEY_k
#define KEY_RIGHT         XKB_KEY_l
#define KEY_FIRST         XKB_KEY_g  /* Press twice */
#define KEY_LAST          XKB_KEY_G
#define KEY_HELP          XKB_KEY_question
#define KEY_LAUNCH        XKB_KEY_Return
#define KEY_ADD           XKB_KEY_plus
#define KEY_EDIT          XKB_KEY_e
#define KEY_REFRESH       XKB_KEY_r
#define KEY_RENAME        XKB_KEY_R
#define KEY_REMOVE        XKB_KEY_d
#define KEY_LOG           XKB_KEY_L
#define KEY_QUIT          XKB_KEY_q
#define KEY_SEARCH        XKB_KEY_slash
#define KEY_NEXT_MATCH    XKB_KEY_n
#define KEY_PREV_MATCH    XKB_KEY_N
