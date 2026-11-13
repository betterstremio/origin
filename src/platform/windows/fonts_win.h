/* Windows' own fonts in C:/Windows/Fonts, by exact file name (ui/text.c includes this for SB_FONT_TABLE): the bundled
   faces cover Latin, Greek, Cyrillic and Arabic; these cover the rest. Files a Windows version lacks are skipped. Only
   these are looked at, never the whole folder, which can hold hundreds. */
static const FbSpec PLATFONTS[] = {
  { "seguisym.ttf", C_SYM },
  { "seguiemj.ttf", C_EMOJI },
  { "YuGothR.ttc", C_KANA | C_HAN },
  { "msyh.ttc", C_HAN },
  { "malgun.ttf", C_HANGUL },
  { "segoeui.ttf", C_GEN, { HB_SCRIPT_HEBREW, HB_SCRIPT_ARMENIAN, HB_SCRIPT_GEORGIAN } },
  { "LeelawUI.ttf", 0, { HB_SCRIPT_THAI, HB_SCRIPT_LAO, HB_SCRIPT_KHMER } },
  { "Nirmala.ttc", 0, { HB_SCRIPT_DEVANAGARI, HB_SCRIPT_BENGALI, HB_SCRIPT_TAMIL } }, /* Windows 11 */
  { "Nirmala.ttf", 0, { HB_SCRIPT_DEVANAGARI, HB_SCRIPT_BENGALI, HB_SCRIPT_TAMIL } }, /* Windows 10 */
  { "Nirmala.ttc", 0, { HB_SCRIPT_TELUGU, HB_SCRIPT_GUJARATI, HB_SCRIPT_GURMUKHI } },
  { "Nirmala.ttf", 0, { HB_SCRIPT_TELUGU, HB_SCRIPT_GUJARATI, HB_SCRIPT_GURMUKHI } },
  { "Nirmala.ttc", 0, { HB_SCRIPT_KANNADA, HB_SCRIPT_MALAYALAM, HB_SCRIPT_ORIYA } },
  { "Nirmala.ttf", 0, { HB_SCRIPT_KANNADA, HB_SCRIPT_MALAYALAM, HB_SCRIPT_ORIYA } },
  { "Nirmala.ttc", 0, { HB_SCRIPT_SINHALA } },
  { "Nirmala.ttf", 0, { HB_SCRIPT_SINHALA } },
  { "mmrtext.ttf", 0, { HB_SCRIPT_MYANMAR } },
  { "ebrima.ttf", 0, { HB_SCRIPT_ETHIOPIC } },
};
