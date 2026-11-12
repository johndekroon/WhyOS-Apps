"""What libwb draws with: every font, icon and glow in one place.

gen.py renders these into the headers; run it after changing anything here
(README.md, "Changing it"). A program links only what it uses, so an entry
costs the others nothing.

Add an icon here rather than in an app, and keep to the drawing style: a
24 x 24 viewBox, round caps and joins, the size and stroke width of its group.
"""
UI = "SpaceGrotesk-VF.ttf"
MONO = "JetBrainsMono-VF.ttf"
EMOJI_FONT = "NotoEmoji-VF.ttf"

# (C name, file, size px, weight). Sizes from the design's screens.
FONTS = [
    ("f_ui16", UI, 16, 400),      # hint bar
    ("f_ui18", UI, 18, 400),      # tile labels, list values
    ("f_ui18m", UI, 18, 500),     # focused tile label
    ("f_ui18b", UI, 18, 700),     # status bar: WhyOS
    ("f_ui20", UI, 20, 400),      # date
    ("f_ui22", UI, 22, 400),      # list rows, dialog buttons
    ("f_ui28b", UI, 28, 700),     # dialog title
    ("f_ui36b", UI, 36, 700),     # list screen title
    ("f_mono13", MONO, 13, 400),  # key caps, group labels
    ("f_mono14", MONO, 14, 400),  # eyebrow
    ("f_mono16b", MONO, 16, 600), # status bar clock
    ("f_mono18", MONO, 18, 400),  # list values (percentages)
    # appterm's terminal: 15 px is a 9 px cell; box drawing, blocks and a
    # few symbols CLI programs print besides ASCII
    ("f_term15", MONO, 15, 400, "TERM"),
]
FIRST, LAST = 0x20, 0x7E
EXTRA = "\u2190\u2191\u2192\u2193\u2026\u2022\u232b\u00b0\u00b7\u2013\u00b2\u00b5\u203a"   # arrows, ellipsis, bullet, erase key, degree, middle dot, en dash, ², µ, ›
TERM_EXTRA = EXTRA + ("\u00a9\u00ae\u00b1\u00d7"
                      "\u2500\u2502\u250c\u2510\u2514\u2518\u251c\u2524\u252c\u2534\u253c"
                      "\u2550\u2551\u2554\u2557\u255a\u255d\u2588\u2580\u2584\u2591\u2592\u2593"
                      "\u25b2\u25bc\u25c6\u2713\u2717")


# The document fonts (wb_doc_fonts.h, wbdoc.h): for text people type. The
# fonts above have ASCII and a few symbols; these have Latin-1 and the
# punctuation people type too, at the sizes of Write's design, with 4 bits of
# alpha: half the flash of 8 bits, and the difference does not show at these
# sizes. Space Grotesk has no italic: wbdoc.h slants the regular glyphs.
DOC_CHARS = ([chr(c) for c in range(0x20, 0x7f)] + [chr(c) for c in range(0xa1, 0x100)] +
             list("–—‘’‚“”„•…€™"
                  "←↑→↓− "))
DOC_BLANK = " \u00a0"
DOC_FONTS = [
    ("wd_ui24", UI, 24, 400), ("wd_ui24b", UI, 24, 700), ("wd_ui36b", UI, 36, 700),
    ("wd_ui28b", UI, 28, 700), ("wd_ui20", UI, 20, 400), ("wd_ui20b", UI, 20, 700),
    ("wd_mono20", MONO, 20, 400), ("wd_mono18", MONO, 18, 400), ("wd_mono16", MONO, 16, 400),
    ("wd_mono14", MONO, 14, 400),
]

# (C name, size px, stroke width, SVG inner elements). Paths verbatim from
# the design where it has them.
ICONS = [
    # home tiles, 44 px, 1.6
    ("i_apps", 44, 1.6, '<path d="M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z"/>'),
    ("i_games", 44, 1.6, '<path d="M6 8h12a4 4 0 0 1 4 4v1a4 4 0 0 1-7 2.6L14 15h-4l-1 0.6A4 4 0 0 1 2 13v-1a4 4 0 0 1 4-4zM8 10v4M6 12h4M15.5 11.5h.01M17.5 13.5h.01"/>'),
    ("i_terminal", 44, 1.6, '<path d="M3 5h18v14H3zM7 10l3 2-3 2M12 14h5"/>'),
    ("i_settings", 44, 1.6, '<path d="M4 7h10M18 7h2M4 17h4M12 17h8M16 5v4M10 15v4"/>'),
    ("i_power", 44, 1.6, '<path d="M12 3v8M7.05 6.5a7 7 0 1 0 9.9 0"/>'),
    # symbol keys: tile corner 14 px and key caps 12 px, 2.6
    ("k_square", 14, 2.6, '<path d="M6 6h12v12H6z"/>'),
    ("k_triangle", 14, 2.6, '<path d="M12 5l7.5 13h-15z"/>'),
    ("k_cross", 14, 2.6, '<path d="M6 6l12 12M18 6L6 18"/>'),
    ("k_circle", 14, 2.6, '<path d="M5 12a7 7 0 1 0 14 0a7 7 0 1 0-14 0"/>'),
    ("k_cloud", 14, 2.6, '<path d="M9 7.5a3 3 0 1 0 6 0a3 3 0 1 0-6 0M4.5 14.5a3 3 0 1 0 6 0a3 3 0 1 0-6 0M13.5 14.5a3 3 0 1 0 6 0a3 3 0 1 0-6 0"/>'),
    ("k_diamond", 14, 2.6, '<path d="M12 4l8 8-8 8-8-8z"/>'),
    ("kc_square", 12, 2.6, '<path d="M6 6h12v12H6z"/>'),
    ("kc_triangle", 12, 2.6, '<path d="M12 5l7.5 13h-15z"/>'),
    ("kc_cross", 12, 2.6, '<path d="M6 6l12 12M18 6L6 18"/>'),
    ("kc_circle", 12, 2.6, '<path d="M5 12a7 7 0 1 0 14 0a7 7 0 1 0-14 0"/>'),
    ("kc_cloud", 12, 2.6, '<path d="M9 7.5a3 3 0 1 0 6 0a3 3 0 1 0-6 0M4.5 14.5a3 3 0 1 0 6 0a3 3 0 1 0-6 0M13.5 14.5a3 3 0 1 0 6 0a3 3 0 1 0-6 0"/>'),
    ("kc_diamond", 12, 2.6, '<path d="M12 4l8 8-8 8-8-8z"/>'),
    # the logo key, as the WHY2025 logo on it: a circle with a ring across it, 16 px
    ("kc_logo", 16, 1.9, '<circle cx="12" cy="12" r="7.6"/>'
                         '<ellipse cx="12" cy="12" rx="11.4" ry="3.2" transform="rotate(20 12 12)"/>'),
    # status bar
    ("i_thermo20", 20, 2, '<path d="M10 4v10a3.5 3.5 0 1 0 4 0V4a2 2 0 0 0-4 0z"/><path d="M12 11v5"/>'),
    ("i_wifi22", 22, 2, '<path d="M2 9a15 15 0 0 1 20 0"/><path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    # the status bar's signal strength: lit from the dot outwards, the rest
    # dimmed with i_wifi22 underneath (wifi_state.h)
    ("i_sig0_22", 22, 2, '<circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    ("i_sig1_22", 22, 2, '<path d="M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    ("i_sig2_22", 22, 2, '<path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    # list rows and dialog, 26 px, 2
    ("r_wifi", 26, 2, '<path d="M2 9a15 15 0 0 1 20 0"/><path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 16a5 5 0 0 1 7 0"/>'),
    ("r_sun", 26, 2, '<circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M4.9 19.1L7 17M17 7l2.1-2.1"/>'),
    ("r_keyboard", 26, 2, '<rect x="3" y="7" width="18" height="10" rx="2"/><path d="M7 11h2M11 11h2M15 11h2M8 14h8"/>'),
    ("r_restart", 26, 2, '<path d="M20 12a8 8 0 1 1-2.34-5.66"/><path d="M20 4v5h-5"/>'),
    ("r_power", 26, 2, '<path d="M12 3v8"/><path d="M7.05 6.5a7 7 0 1 0 9.9 0"/>'),
    ("r_moon", 26, 2, '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>'),
    ("r_chevron", 20, 2, '<path d="M9 6l6 6-6 6"/>'),
    # rows the design has no icon for; drawn in the same style
    ("r_bluetooth", 26, 2, '<path d="M7 7l10 10-5 4V3l5 4L7 17"/>'),
    ("r_lora", 26, 2, '<path d="M12 3v5M12 16v5M4 7.5l4.3 2.5M15.7 14l4.3 2.5M4 16.5L8.3 14M15.7 10L20 7.5"/>'),
    ("r_sensors", 26, 2, '<path d="M3 12h4l3-8 4 16 3-8h4"/>'),
    ("r_usb", 26, 2, '<path d="M12 3v14M9 6l3-3 3 3M7 11v2l5 3M17 9v3l-5 3"/><circle cx="12" cy="19" r="2"/>'),
    ("r_games", 26, 2, '<path d="M6 8h12a4 4 0 0 1 4 4v1a4 4 0 0 1-7 2.6L14 15h-4l-1 0.6A4 4 0 0 1 2 13v-1a4 4 0 0 1 4-4zM8 10v4M6 12h4"/>'),
    ("r_info", 26, 2, '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5h.01"/>'),
    # network manager: signal strength as the design's Wi-Fi arcs, lit from
    # the dot outwards; the dimmed rest is drawn with r_wifi underneath
    ("r_sig0", 26, 2, '<circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    ("r_sig1", 26, 2, '<path d="M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    ("r_sig2", 26, 2, '<path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    ("r_sig3", 26, 2, '<path d="M2 9a15 15 0 0 1 20 0"/><path d="M5 12.5a10 10 0 0 1 14 0"/><path d="M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1" fill="#fff"/>'),
    # sound (the sound UI's design): the speaker, muted (or out of reach), no
    # output, AirPlay; 22 px for the status bar, 26 for rows
    ("i_spk22", 22, 2, '<path d="M4 9h4l5-4v14l-5-4H4zM16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"/>'),
    ("i_spkmute22", 22, 2, '<path d="M4 9h4l5-4v14l-5-4H4zM17 9l5 6M22 9l-5 6"/>'),
    ("i_spknone22", 22, 2, '<path d="M4 9h4l5-4v14l-5-4H4zM3 3l18 18"/>'),
    ("i_airplay22", 22, 2, '<path d="M6 17H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-2M12 15l5 6H7z"/>'),
    ("r_spk", 26, 2, '<path d="M4 9h4l5-4v14l-5-4H4zM16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"/>'),
    ("r_spkmute", 26, 2, '<path d="M4 9h4l5-4v14l-5-4H4zM17 9l5 6M22 9l-5 6"/>'),
    ("r_spknone", 26, 2, '<path d="M4 9h4l5-4v14l-5-4H4zM3 3l18 18"/>'),
    ("r_airplay", 26, 2, '<path d="M6 17H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-2M12 15l5 6H7z"/>'),
    ("r_sliders", 26, 2, '<path d="M4 7h10M18 7h2M4 17h4M12 17h8M16 5v4M10 15v4"/>'),
    ("r_lock", 18, 2, '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>'),
    ("r_ethernet", 26, 2, '<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M7 16v4h10v-4M8 8v3M11 8v3M14 8v3M17 8v3"/>'),
    ("r_refresh", 26, 2, '<path d="M20 12a8 8 0 1 1-2.34-5.66"/><path d="M20 4v5h-5"/>'),
    ("r_trash", 26, 2, '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>'),
    ("r_sdcard", 26, 2, '<path d="M7 3h8l4 4v14H7z"/><path d="M10 6v3M13 6v3M16 9v0"/>'),
    ("r_drive", 26, 2, '<rect x="3" y="6" width="18" height="12" rx="2"/><path d="M7 14h.01M11 14h6"/>'),
    ("r_check", 26, 2, '<path d="M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z"/><path d="M8.5 12l2.5 2.5 4.5-5"/>'),
    ("r_ip", 26, 2, '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M8 9v6M12 15V9h2.5a2 2 0 0 1 0 4H12"/>'),
    ("r_usbstick", 26, 2, '<path d="M8 2h8v6H8z"/><path d="M6 8h12v11a3 3 0 0 1-3 3H9a3 3 0 0 1-3-3z"/><path d="M10.5 5h.01M13.5 5h.01"/>'),
    # more rows, and the names an app's ICON can give
    ("r_chip", 26, 2, '<rect x="6" y="6" width="12" height="12" rx="1"/><path d="M9 3v3M12 3v3M15 3v3M9 18v3M12 18v3M15 18v3M3 9h3M3 12h3M3 15h3M18 9h3M18 12h3M18 15h3"/>'),
    ("r_bolt", 26, 2, '<path d="M13 2L4 14h7l-1 8 9-12h-7z"/>'),
    ("r_led", 26, 2, '<path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-3.5 10.9c.6.5 1 1.2 1 2V16h5v-.1c0-.8.4-1.5 1-2A6 6 0 0 0 12 3z"/>'),
    ("r_palette", 26, 2, '<path d="M12 3a9 9 0 0 0 0 18c1.5 0 2-1 2-2s-1-1.5-1-2.5S14 15 15 15h2a4 4 0 0 0 4-4c0-4.5-4-8-9-8z"/><path d="M7.5 11h.01M10 7h.01M15 7.5h.01"/>'),
    ("r_count", 26, 2, '<path d="M4 7h16M4 12h16M4 17h10"/>'),
    # Files: the tile, and the browser's rows
    ("i_files", 44, 1.6, '<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/>'),
    ("r_folder", 26, 2, '<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/>'),
    ("r_file", 26, 2, '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/>'),
    ("r_text", 26, 2, '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5M9 13h6M9 17h6"/>'),
    ("r_image", 26, 2, '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="M21 16l-5-5-9 9"/>'),
    ("r_doc", 26, 2, '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/><rect x="8.5" y="11.5" width="7" height="6" rx="0.5"/>'),
    # the app store's rows (Apps, Settings > Storage & devices)
    ("r_store", 26, 2, '<path d="M6 2L3 6v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V6l-3-4z"/><path d="M3 6h18M16 10a4 4 0 0 1-8 0"/>'),
    # Settings > General
    ("r_user", 26, 2, '<circle cx="12" cy="8" r="4"/><path d="M4 21v-1a6 6 0 0 1 6-6h4a6 6 0 0 1 6 6v1"/>'),
    # apps from the SD card
    ("r_globe", 26, 2, '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>'),
    ("r_gameboy", 26, 2, '<rect x="6" y="2" width="12" height="20" rx="2"/><rect x="8.5" y="4.5" width="7" height="6" rx="0.5"/><path d="M9 15v3M7.5 16.5h3M14.5 17h.01M16 15.5h.01"/>'),
    # BARbarious (sdapps/barbarious): its codes by kind, as its design draws them
    ("r_qr", 26, 2, '<path d="M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h2v2h-2zM18 18h2v2h-2zM18 14h2M14 18v2"/>'),
    ("r_barcode", 26, 2, '<path d="M4 5v14M7 5v14M10 5v14M12 5v14M16 5v14M18 5v14M20 5v14"/>'),
    ("r_matrix", 26, 2, '<path d="M4 4v16h16M8 7h2v2H8zM12 11h2v2h-2zM16 7h2v2h-2zM8 15h2v2H8zM16 15h2v2h-2z"/>'),
    # OHMinous (sdapps/ohminous): its tiles (34 px), the two sources, the buses, Power's rows
    ("t_pins", 34, 1.6, '<path d="M3 10h18v4H3zM6 10V5M10 10V5M14 10V5M18 10V5"/>'),
    ("t_logic", 34, 1.6, '<path d="M2 16h4V8h4v8h4V8h4v8h4"/>'),
    ("t_buses", 34, 1.6, '<path d="M3 8h18M3 16h18M8 8v8M16 16v-8"/>'),
    ("t_leds", 34, 1.6, '<path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-3.5 10.9V16h7v-2.1A6 6 0 0 0 12 3z"/>'),
    ("t_power", 34, 1.6, '<path d="M13 3L5 14h6l-1 7 8-11h-6z"/>'),
    ("t_serial", 34, 1.6, '<path d="M3 5h18v14H3zM7 10l3 2-3 2M12 14h5"/>'),
    ("t_plug", 34, 1.6, '<path d="M9 3v5M15 3v5M7 8h10v3a5 5 0 0 1-10 0zM12 16v5"/>'),
    ("o_badge", 26, 1.8, '<path d="M7 7h10v10H7zM10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4"/>'),
    ("o_badge20", 20, 1.8, '<path d="M7 7h10v10H7zM10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4"/>'),
    ("o_plug", 26, 1.8, '<path d="M9 3v5M15 3v5M7 8h10v3a5 5 0 0 1-10 0zM12 16v5"/>'),
    ("o_plug72", 72, 1.2, '<path d="M9 3v5M15 3v5M7 8h10v3a5 5 0 0 1-10 0zM12 16v5"/>'),
    ("b_i2c", 30, 1.8, '<path d="M3 8h18M3 16h18M8 8v8M16 16v-8"/>'),
    ("b_uart", 30, 1.8, '<path d="M4 8h13l-3-3M20 16H7l3 3"/>'),
    ("b_spi", 30, 1.8, '<path d="M7 4h10v16H7zM4 8h3M4 12h3M4 16h3M17 8h3M17 12h3M17 16h3"/>'),
    ("b_1wire", 30, 1.8, '<path d="M3 12h6M15 12h6M9 12a3 3 0 1 0 6 0a3 3 0 1 0-6 0"/>'),
    ("p_power", 24, 2, '<path d="M12 3v8M7.05 6.5a7 7 0 1 0 9.9 0"/>'),
    ("p_pullup", 24, 2, '<path d="M12 3v4M8 7h8v4H8zM10 11v3l-2 2 2 2-2 2M14 11v10"/>'),
    ("s_warn", 16, 2.2, '<path d="M12 4l9 16H3zM12 10v4M12 17h.01"/>'),
    ("s_up", 14, 2.2, '<path d="M12 19V5M6 11l6-6 6 6"/>'),
    # Write (WhyOS-Apps write/): the sizes of its screens
    ("wi_plus", 22, 1.9, '<path d="M12 5v14M5 12h14"/>'),
    ("wi_folder", 22, 1.9, '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>'),
    ("wi_doc", 22, 1.9, '<path d="M6 3h8l4 4v14H6zM14 3v4h4M9 12h6M9 16h6"/>'),
    ("wi_code", 22, 1.9, '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5"/>'),
    ("wi_code14", 14, 2.2, '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5"/>'),
    ("wi_chev", 18, 2, '<path d="M9 6l6 6-6 6"/>'),
    ("wi_check", 20, 2.4, '<path d="M5 12.5l4.5 4.5L19 7.5"/>'),
    ("wi_trash", 20, 2, '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>'),
    ("wi_copy", 12, 2.2, '<path d="M9 9h11v11H9zM5 15H4V4h11v1"/>'),
    ("wi_triangle", 12, 2.8, '<path d="M12 4l9 16H3z"/>'),
    ("wi_drive", 22, 1.9, '<rect x="3" y="6" width="18" height="12" rx="2"/><path d="M7 14h.01M11 14h6"/>'),
]

# Focus glow: (C name, blur px, opacity), see gen_glow()
GLOWS = [("glow_tile", 22, 0.45), ("glow_row", 18, 0.40)]

# The emoji the chat app shows (emoji_font.h, WB_EMOJI in wbdraw.h)
EMOJI_SIZE = 20
# Code points, as ranges (first, last). Faces and gestures in full; from the
# rest what chats use: hearts, hands, celebration, weather, radio and
# camping things, some animals, food and drink, ticks and crosses.
EMOJI = [
    (0x1F600, 0x1F64F), (0x1F910, 0x1F92F), (0x1F970, 0x1F97A), (0x1F9D0, 0x1F9D0),
    (0x1F440, 0x1F440), (0x1F442, 0x1F442), (0x1F446, 0x1F450), (0x1F4AA, 0x1F4AA),
    (0x1F590, 0x1F590), (0x1F595, 0x1F596), (0x1F918, 0x1F91F), (0x1F932, 0x1F932),
    (0x1F937, 0x1F937), (0x1F926, 0x1F926), (0x270A, 0x270D), (0x261D, 0x261D),
    (0x2764, 0x2764), (0x2763, 0x2763), (0x1F493, 0x1F49F), (0x1F5A4, 0x1F5A4),
    (0x1F90D, 0x1F90E), (0x1F9E1, 0x1F9E1), (0x1F48B, 0x1F48B),
    (0x1F525, 0x1F525), (0x1F389, 0x1F38A), (0x1F381, 0x1F382), (0x1F388, 0x1F388),
    (0x2728, 0x2728), (0x2B50, 0x2B50), (0x1F31F, 0x1F31F), (0x1F4AF, 0x1F4AF),
    (0x1F4A1, 0x1F4A1), (0x1F4A4, 0x1F4A6), (0x1F4A8, 0x1F4A9), (0x1F4A5, 0x1F4A5),
    (0x1F4AC, 0x1F4AC), (0x1F480, 0x1F480), (0x1F47B, 0x1F47D), (0x1F916, 0x1F916),
    (0x2705, 0x2705), (0x2714, 0x2714), (0x2716, 0x2716), (0x274C, 0x274C),
    (0x2753, 0x2753), (0x2757, 0x2757), (0x203C, 0x203C), (0x2049, 0x2049),
    (0x26A0, 0x26A0), (0x26A1, 0x26A1), (0x1F6A8, 0x1F6A8), (0x1F198, 0x1F198),
    (0x1F6A9, 0x1F6A9), (0x1F3C1, 0x1F3C1), (0x1F4CD, 0x1F4CD), (0x1F5FA, 0x1F5FA),
    (0x2600, 0x2601), (0x26C5, 0x26C5), (0x1F327, 0x1F327), (0x2744, 0x2744),
    (0x1F308, 0x1F308), (0x1F319, 0x1F319), (0x1F30D, 0x1F310), (0x1F30A, 0x1F30A),
    (0x1F4E1, 0x1F4E1), (0x1F4F6, 0x1F4F6), (0x1F50B, 0x1F50B), (0x1F50C, 0x1F50C),
    (0x1F4FB, 0x1F4FB), (0x1F4F1, 0x1F4F1), (0x1F4BB, 0x1F4BB), (0x1F4F7, 0x1F4F7),
    (0x1F4DE, 0x1F4DE), (0x1F4E2, 0x1F4E2), (0x1F4E9, 0x1F4E9), (0x1F514, 0x1F514),
    (0x1F50A, 0x1F50A), (0x1F511, 0x1F512), (0x1F6F0, 0x1F6F0), (0x1F680, 0x1F680),
    (0x2708, 0x2708), (0x1F697, 0x1F697), (0x1F6B2, 0x1F6B2), (0x1F3E0, 0x1F3E0),
    (0x26FA, 0x26FA), (0x1F3D5, 0x1F3D5), (0x1F3B5, 0x1F3B6), (0x1F3AE, 0x1F3AE),
    (0x1F947, 0x1F947), (0x1F3C6, 0x1F3C6), (0x23F0, 0x23F0), (0x231B, 0x231B),
    (0x1F436, 0x1F436), (0x1F431, 0x1F431), (0x1F98A, 0x1F98A), (0x1F43B, 0x1F43B),
    (0x1F427, 0x1F427), (0x1F986, 0x1F986), (0x1F41D, 0x1F41D), (0x1F984, 0x1F984),
    (0x1F338, 0x1F338), (0x1F333, 0x1F333), (0x1F340, 0x1F340), (0x1F335, 0x1F335),
    (0x2615, 0x2615), (0x1F37A, 0x1F37B), (0x1F377, 0x1F377), (0x1F37E, 0x1F37E),
    (0x1F964, 0x1F964), (0x1F355, 0x1F355), (0x1F354, 0x1F354), (0x1F35F, 0x1F35F),
    (0x1F366, 0x1F366), (0x1F369, 0x1F36A), (0x1F34E, 0x1F34E), (0x1F34C, 0x1F34C),
    (0x1F951, 0x1F951), (0x1F32D, 0x1F32D),
]
