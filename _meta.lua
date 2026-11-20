local _ = require("gettext")
return {
    name = "multidict",
    fullname = _("Multi-format dictionaries"),
    description = _([[Look up words in MDict (.mdx, including 3.0), DSL (Lingvo), dictd, XDXF, TSV, CSV, StarDict textual (.xml), TangoDict (.dc), mdict-utils SQLite (.db), Kobo (dicthtml-*.zip), Aard 2 (.slob), Zim (Kiwix), QuickDic 6, PocketBook SDIC (.dic), Apple Dictionary (.dictionary), Babylon (.bgl), dictionary EPUB and Mobipocket/Kindle (.mobi, .prc, .azw) dictionaries directly, next to StarDict ones, using a native library (libmultidict). Place the files in the same dictionary folder as your StarDict dictionaries.]]),
}
