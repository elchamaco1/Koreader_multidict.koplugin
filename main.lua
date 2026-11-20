--[[--
Multi-format dictionaries for KOReader.

Adds MDict (.mdx), DSL/DSL.dz (ABBYY Lingvo), dictd (.index + .dict[.dz]),
XDXF, TSV/tab, CSV, StarDict textual (.xml), TangoDict (.dc), mdict-utils
SQLite (.db), Kobo (dicthtml-*.zip), Aard 2 (.slob), Zim (Kiwix), QuickDic 6,
PocketBook SDIC (.dic), Apple Dictionary (.dictionary), Babylon (.bgl),
dictionary EPUB and Mobipocket/Kindle (.mobi/.prc/.azw) dictionaries to
KOReader's normal dictionary lookup.

It works the same way as sdcv does for StarDict: a small native library
(libmultidict.so, loaded through LuaJIT FFI) reads each entry directly from
the dictionary file using the dictionary's own index. Formats that have no
usable index of their own (DSL, XDXF, TSV, CSV, StarDict textual, Babylon,
EPUB, Apple) get a small sorted .mdidx index built once, the first time they
are opened.

The dictionaries appear in the regular "Dictionary settings" list, so they
can be ordered, disabled, used in presets, etc.

@module koplugin.multidict
--]]

local DataStorage = require("datastorage")
local InfoMessage = require("ui/widget/infomessage")
local ReaderDictionary = require("apps/reader/modules/readerdictionary")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")
local _ = require("gettext")
local T = require("ffi/util").template

local plugin_dir = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "plugins/multidict.koplugin"
local MD = dofile(plugin_dir .. "/multidict_ffi.lua")
local lib_ok = MD.load(plugin_dir)
if not lib_ok then
    logger.warn("multidict: cannot load libmultidict:", MD.load_error)
end

local cache_dir = DataStorage:getDataDir() .. "/cache/multidict"

-- Module state shared by every ReaderUI / FileManager instance
local opened = {}       -- path -> Dict object (kept open, like sdcv keeps its .idx)
local failed = {}       -- path -> error message
local by_name = {}      -- dictionary name -> Dict object
local registered = setmetatable({}, { __mode = "k" }) -- available_ifos tables we already extended

-------------------------------------------------------------------------------
-- helpers

local function getUpvalue(func, wanted)
    local i = 1
    while true do
        local name, value = debug.getupvalue(func, i)
        if not name then return nil end
        if name == wanted then return value end
        i = i + 1
    end
end

local function readFile(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local content = f:read("*all")
    f:close()
    return content
end

local function stripExt(path)
    for _i, ext in ipairs{ "%.dict%.dz$", "%.dsl%.dz$", "%.xdxf%.dz$", "%.tab%.dz$", "%.[^./]+$" } do
        local s, n = path:gsub(ext, "")
        if n > 0 then return s end
    end
    return path
end

local function findDictFiles(dir, out)
    local ok, iter, dir_obj = pcall(lfs.dir, dir)
    if not ok then return out end
    for name in iter, dir_obj do
        if name ~= "." and name ~= ".." and name ~= "res" then
            local full = dir .. "/" .. name
            local mode = lfs.attributes(full, "mode")
            if mode == "directory" then
                findDictFiles(full, out)
            elseif mode == "file" and not name:match("%.mdidx$") and MD.probe(full) then
                table.insert(out, full)
            end
        end
    end
    return out
end

local function langCode(s)
    if s and s:match("^%a%a%a?$") then return s:lower() end
end

--- Opens every supported dictionary found in the dictionary folders.
local function openAll(data_dir)
    local dirs = { data_dir }
    if lfs.attributes(data_dir .. "_ext", "mode") == "directory" then
        table.insert(dirs, data_dir .. "_ext")
    end
    local files = {}
    for _i, d in ipairs(dirs) do findDictFiles(d, files) end

    for _i, path in ipairs(files) do
        if not opened[path] and not failed[path] then
            local msg
            if MD.needsIndex(path, cache_dir) then
                lfs.mkdir(DataStorage:getDataDir() .. "/cache")
                lfs.mkdir(cache_dir)
                msg = InfoMessage:new{
                    text = T(_("Indexing dictionary (only the first time):\n%1"), path:match("[^/]+$")),
                }
                UIManager:show(msg)
                UIManager:forceRePaint()
            end
            local d, err = MD.open(path, cache_dir)
            if msg then UIManager:close(msg) end
            if d then
                opened[path] = d
                logger.info("multidict: opened", d.format, d.name, d.word_count)
            else
                failed[path] = err
                logger.warn("multidict: cannot open", path, err)
            end
        end
    end
    return files
end

--- Adds our dictionaries to ReaderDictionary's list of available dictionaries.
local function registerInto(rd)
    if not lib_ok then return end
    local ifos = getUpvalue(ReaderDictionary.init, "available_ifos")
    if type(ifos) ~= "table" or registered[ifos] then return end
    registered[ifos] = true

    local existing = {}
    for _i, ifo in ipairs(ifos) do existing[ifo.name] = true end

    local files = openAll(rd.data_dir)
    by_name = {}
    for _i, path in ipairs(files) do
        local d = opened[path]
        if d then
            -- Nombres repetidos (p. ej. varios dicthtml-es.zip en subcarpetas):
            -- se distinguen por la carpeta y, si aún coinciden, por un número.
            local name = d.name
            if existing[name] then
                local folder = path:match("([^/]+)%.dictionary/") -- paquete de Apple
                    or path:match("([^/]+)/[^/]+$")
                if folder and folder ~= "dict" and folder ~= "dict_ext" then
                    name = d.name .. " (" .. folder .. ")"
                end
                local n = 2
                local candidate = name
                while existing[candidate] do
                    candidate = name .. " " .. n
                    n = n + 1
                end
                name = candidate
            end
            existing[name] = true
            d.display_name = name
            by_name[name] = d
            local base = stripExt(path)
            local fix_func
            local fix_path = base .. ".lua"
            if lfs.attributes(fix_path, "mode") == "file" then
                local ok, func = pcall(dofile, fix_path)
                if ok and type(func) == "function" then fix_func = func end
            end
            local lin, lout = langCode(d.lang_from), langCode(d.lang_to)
            table.insert(ifos, {
                file = path,
                name = name,
                is_html = d.is_html,
                css = readFile(base .. ".css"),
                fix_html_func = fix_func,
                lang = lin and { lang_in = lin, lang_out = lout or "" } or nil,
                multidict = true,
            })
        end
    end
    rd:sortAvailableIfos()
    rd:updateSdcvDictNamesOptions()
end

-------------------------------------------------------------------------------
-- ReaderDictionary hooks (installed once)

if lib_ok and not ReaderDictionary._multidict_patched then
    ReaderDictionary._multidict_patched = true

    local orig_init = ReaderDictionary.init
    ReaderDictionary.init = function(self, ...)
        orig_init(self, ...)
        -- the list may have been (re)built by this init: add ours again
        registerInto(self)
    end
    -- debug.getupvalue must look at the original function
    local getUpvalueOrig = getUpvalue
    getUpvalue = function(func, wanted)
        if func == ReaderDictionary.init then func = orig_init end
        return getUpvalueOrig(func, wanted)
    end

    local orig_rawSdcv = ReaderDictionary.rawSdcv
    ReaderDictionary.rawSdcv = function(self, words, dict_names, fuzzy_search, lookup_progress_msg)
        -- Split the requested dictionaries between sdcv (StarDict) and us
        local ours, sd_names = {}, nil
        if dict_names then
            sd_names = {}
            for _i, name in ipairs(dict_names) do
                if by_name[name] then
                    table.insert(ours, by_name[name])
                else
                    table.insert(sd_names, name)
                end
            end
        else
            local disabled = G_reader_settings:readSetting("dicts_disabled") or {}
            for name, d in pairs(by_name) do
                if not disabled[d.path] then table.insert(ours, d) end
            end
        end

        local cancelled, results = false, nil
        -- an empty -u list would make sdcv use *all* its dictionaries: skip it then
        if sd_names == nil or #sd_names > 0 then
            cancelled, results = orig_rawSdcv(self, words, sd_names, fuzzy_search, lookup_progress_msg)
        end
        if #ours == 0 or cancelled then
            return cancelled, results
        end

        results = results or {}
        local max_per_dict = fuzzy_search and 8 or 16
        for i, word in ipairs(words) do
            local list = results[i] or {}
            results[i] = list
            for _i, d in ipairs(ours) do
                local ok, res = pcall(d.lookup, d, word, fuzzy_search, max_per_dict)
                if ok then
                    for _i, r in ipairs(res) do
                        table.insert(list, {
                            dict = d.display_name or d.name,
                            word = r.word,
                            definition = r.definition,
                        })
                    end
                else
                    logger.warn("multidict: lookup failed in", d.name, res)
                end
            end
        end

        -- Keep the user's dictionary order (the order of the -u options)
        local rank = {}
        for idx, name in ipairs(dict_names or {}) do rank[name] = idx end
        for _i, list in ipairs(results) do
            for idx, r in ipairs(list) do r._pos = idx end
            table.sort(list, function(a, b)
                local ra, rb = rank[a.dict] or math.huge, rank[b.dict] or math.huge
                if ra ~= rb then return ra < rb end
                return a._pos < b._pos
            end)
            for _i, r in ipairs(list) do r._pos = nil end
        end
        return cancelled, results
    end
end

-------------------------------------------------------------------------------
-- Links with an anchor: "bword://word#id", "word#id" and "#id".
-- KOReader looks up "word#id" literally (no result) and ignores links inside
-- the same definition. Here the anchor is removed from the looked-up word and,
-- once the definition is shown, the window scrolls to the element with that
-- id (resolved by MuPDF itself, the engine that renders the definition).
-- Works for every HTML dictionary, StarDict ones included.

local DictQuickLookup = require("ui/widget/dictquicklookup")

if not DictQuickLookup._multidict_anchor_patched then
    DictQuickLookup._multidict_anchor_patched = true
    local ffi = require("ffi")
    local W -- MuPDF wrapper (already loaded by KOReader to render HTML)

    -- 0-based page of element `id` in the HTML document of an HtmlBoxWidget
    local function anchorPage(htmlbox, id)
        local doc = htmlbox and htmlbox.document
        if not (doc and doc.doc and doc.ctx) then return end
        if W == nil then
            local ok, lib = pcall(function()
                require("ffi/mupdf") -- declares fz_location and the wrapper functions
                if not ffi.loadlib then require("ffi/loadlib") end
                return ffi.loadlib("wrap-mupdf")
            end)
            W = ok and lib or false
            if not ok then logger.warn("multidict: cannot resolve anchors:", lib) end
        end
        if not W then return end
        local ok, page = pcall(function()
            local pos = ffi.new("float[2]")
            local loc = ffi.new("fz_location")
            W.mupdf_fz_resolve_link(doc.ctx, doc.doc, "#" .. id, pos, pos + 1, loc)
            if loc.chapter == -1 and loc.page == -1 then return nil end
            return W.mupdf_fz_page_number_from_location(doc.ctx, doc.doc, loc)
        end)
        if ok then return page end
    end

    local function scrollToPage(win, page)
        local shw = win and win.is_html and win.shw_widget
        if not (shw and shw.htmlbox_widget and page) then return false end
        local count = shw.htmlbox_widget.page_count
        if not count or count < 1 then return false end
        if page >= count then page = count - 1 end
        if page > 0 then
            shw:scrollToRatio((page + 0.5) / count)
        end
        return true
    end

    local function scrollToAnchor(win, id)
        local shw = win and win.is_html and win.shw_widget
        if not shw then return false end
        return scrollToPage(win, anchorPage(shw.htmlbox_widget, id))
    end

    local function definitionHasId(def, id)
        if type(def) ~= "string" then return false end
        local esc = id:gsub("%p", "%%%0")
        return def:find("id%s*=%s*[\"']" .. esc .. "[\"']") ~= nil
            or def:find("name%s*=%s*[\"']" .. esc .. "[\"']") ~= nil
    end

    local pending -- { id = ..., time = ... } for the window about to open

    local orig_init = DictQuickLookup.init
    DictQuickLookup.init = function(self, ...)
        orig_init(self, ...)
        local p = pending
        pending = nil
        if not (p and os.time() - p.time < 30 and self.results) then return end
        -- show the result that contains the anchor (several entries may match)
        if not definitionHasId(self.definition, p.id) then
            for i, r in ipairs(self.results) do
                if definitionHasId(r.definition, p.id) then
                    self:changeDictionary(i)
                    break
                end
            end
        end
        if not scrollToAnchor(self, p.id) then
            -- the widget may finish its layout later: try again once it is shown
            UIManager:nextTick(function() scrollToAnchor(self, p.id) end)
        end
    end

    local orig_tapped = ReaderDictionary.onHtmlDictionaryLinkTapped
    ReaderDictionary.onHtmlDictionaryLinkTapped = function(self, dictionary, link)
        -- link to another place in the same definition: MuPDF already gives its page
        if link and not link.uri and link.page then
            local list = DictQuickLookup.window_list
            for i = #list, 1, -1 do
                local win = list[i]
                if win.dictionary == dictionary or i == #list then
                    if scrollToPage(win, link.page) then return true end
                end
            end
            return
        end
        if link and link.uri then
            local uri = link.uri
            local prefix = uri:match("^bword://") and "bword://" or ""
            local rest = uri:sub(#prefix + 1)
            if prefix ~= "" or not rest:find("://", 1, true) then
                local word, id = rest:match("^(.-)#(.+)$")
                if id then
                    local list = DictQuickLookup.window_list
                    local top = list[#list]
                    if word == "" then
                        -- "#id" given as uri: scroll in the current window
                        if top and scrollToAnchor(top, id) then return true end
                        return
                    end
                    pending = { id = id, time = os.time() }
                    local copy = {}
                    for k, v in pairs(link) do copy[k] = v end
                    copy.uri = prefix .. word
                    return orig_tapped(self, dictionary, copy)
                end
            end
        end
        return orig_tapped(self, dictionary, link)
    end
end

-------------------------------------------------------------------------------
-- Plugin instance (menu + registration for the already existing dictionary)

local MultiDict = WidgetContainer:extend{
    name = "multidict",
    is_doc_only = false,
}

function MultiDict:init()
    if self.ui and self.ui.menu then
        self.ui.menu:registerToMainMenu(self)
    end
    -- ReaderDictionary is created before plugins are loaded, so the first
    -- list of dictionaries was built without us: extend it now.
    if self.ui and self.ui.dictionary then
        registerInto(self.ui.dictionary)
    end
end

function MultiDict:showStatus()
    local lines = {}
    if not lib_ok then
        table.insert(lines, _("The native library could not be loaded."))
        table.insert(lines, T(_("Architecture: %1"), MD.archDir()))
        table.insert(lines, MD.load_error or "")
    else
        table.insert(lines, T(_("libmultidict %1 (%2)"),
            require("ffi").string(MD.lib.md_version()), MD.archDir()))
        table.insert(lines, "")
        local paths = {}
        for path in pairs(opened) do table.insert(paths, path) end
        table.sort(paths)
        for _i, path in ipairs(paths) do
            local d = opened[path]
            table.insert(lines, T("• %1 [%2] — %3", d.display_name or d.name, d.format,
                d.word_count >= 0 and T(_("%1 entries"), d.word_count) or "?"))
        end
        for path, err in pairs(failed) do
            table.insert(lines, T("✗ %1: %2", path:match("[^/]+$"), err))
        end
        if #paths == 0 and next(failed) == nil then
            table.insert(lines, _("No supported dictionaries found in the dictionary folder."))
        end
    end
    UIManager:show(InfoMessage:new{ text = table.concat(lines, "\n") })
end

function MultiDict:addToMainMenu(menu_items)
    menu_items.multidict = {
        text = _("Multi-format dictionaries"),
        sorting_hint = "more_tools",
        sub_item_table = {
            {
                text = _("Status of loaded dictionaries"),
                keep_menu_open = true,
                callback = function() self:showStatus() end,
            },
            {
                text = _("Rescan dictionary folder"),
                keep_menu_open = true,
                callback = function()
                    failed = {}
                    local ifos = getUpvalue(ReaderDictionary.init, "available_ifos")
                    if type(ifos) == "table" then
                        for i = #ifos, 1, -1 do
                            if ifos[i].multidict then table.remove(ifos, i) end
                        end
                        registered[ifos] = nil
                    end
                    if self.ui and self.ui.dictionary then
                        registerInto(self.ui.dictionary)
                    end
                    self:showStatus()
                end,
            },
        },
    }
end

return MultiDict
