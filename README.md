# Koreader_multidictionary
# Multi-format dictionaries (multidict) para KOReader

Con este plugin, KOReader puede usar diccionarios en casi cualquier formato, no solo StarDict. MDict, Lingvo, Kindle, EPUB, Kobo, PocketBook, Babylon, Apple y muchos más se abren directamente: no hace falta convertirlos antes con pyglossary ni con otro programa.

Los diccionarios del plugin aparecen junto a tus StarDict en la ventana de consulta de siempre. Desde **Ajustes de diccionario** se pueden ordenar, desactivar o meter en preajustes, y sus enlaces internos se siguen como en cualquier otro.

Versión 1.5.0.

---

## Formatos que lee

| Formato | Qué archivo poner en la carpeta |
|---|---|
| MDict (1.2, 2.0 y 3.0) | `.mdx`, con su `.css` si lo tiene |
| ABBYY Lingvo (DSL) | `.dsl` o `.dsl.dz` |
| Mobipocket / Kindle | `.mobi`, `.prc`, `.azw` |
| EPUB de diccionario (EPUB 3 Dictionaries, EPUB 2 de pyglossary, marcas de Kindle) | `.epub` |
| Kobo | `dicthtml-xx.zip` |
| PocketBook | `.dic` (SDIC) |
| Babylon | `.bgl` |
| Apple Dictionary | la carpeta `.dictionary` entera |
| TangoDict (DictTango) | `.dc` |
| SQLite de mdict-utils | `.db` |
| dictd / dict.org | `.index` + `.dict` o `.dict.dz` |
| Aard 2 | `.slob` |
| Zim (Kiwix, p. ej. Wiktionary) | `.zim` |
| QuickDic 6/7 | `.quickdic`, `.quickdic.v006.zip` |
| XDXF | `.xdxf` |
| StarDict textual | `.xml` |
| Tabla de texto | `.tab`, `.tsv`, `.csv` |
| StarDict normal | `.ifo` + `.idx` + `.dict`: lo sigue leyendo KOReader como siempre |

---

## Instalación

### Kobo, Kindle, PocketBook, reMarkable y Linux

1. Descomprime el zip. Saldrá una carpeta llamada `multidict.koplugin`.
2. Cópiala dentro de `koreader/plugins/`.
3. Pon los diccionarios en la carpeta de diccionarios de KOReader, `koreader/data/dict/`. Puedes organizarlos en subcarpetas, por ejemplo una por diccionario.
4. Reinicia KOReader.

### Android

Los pasos son los mismos. La carpeta `koreader/` está en el almacenamiento interno del móvil o tablet, la misma donde KOReader guarda sus ajustes.

Android no deja cargar librerías desde el almacenamiento compartido. Por eso, al arrancar, el plugin copia la suya a la carpeta privada de KOReader y la carga desde ahí. No tienes que hacer nada.

Funciona desde Android 4.3, en procesadores ARM y x86, tanto de 32 como de 64 bits.

### ¿Qué versión de la librería usa?

El plugin elige automáticamente la de tu aparato. Dentro de `libs/` van todas:

| Carpeta | Aparatos |
|---|---|
| `armhf` | Kobo, Kindle con firmware 5.16.3 o posterior, PocketBook, reMarkable 1 |
| `armel` | Kindle con firmware antiguo |
| `arm64` | reMarkable 2 / Paper Pro, Linux ARM de 64 bits |
| `x86_64` | KOReader en un PC con Linux |
| `android-*` | Android (las cuatro arquitecturas) |

Si te falta espacio, puedes borrar las carpetas que no correspondan a tu aparato.

---

## Uso

Mantén pulsada una palabra mientras lees, como siempre. Los resultados de estos diccionarios salen mezclados con los de StarDict, en el orden que hayas elegido en **Ajustes de diccionario**.

En **Herramientas → Más herramientas → Multi-format dictionaries** hay dos opciones:

- **Status of loaded dictionaries:** muestra qué diccionarios se han cargado, cuántas entradas tiene cada uno y, si alguno no ha podido abrirse, el motivo.
- **Rescan dictionary folder:** vuelve a buscar diccionarios en la carpeta. Sirve para añadir o quitar diccionarios sin reiniciar.

### La primera vez que abres algunos diccionarios

La mayoría de los formatos traen su propio índice y se consultan al instante desde el primer momento. Otros no lo traen: DSL, XDXF, TSV/CSV, StarDict textual, Babylon, EPUB y Apple. Con esos, la primera vez aparece el aviso *"Indexing dictionary (only the first time)"* mientras el plugin crea un índice pequeño. Con un diccionario muy grande puede tardar un poco. Después ya no se repite, salvo que cambies el archivo del diccionario.

### Archivos que crea el plugin

Junto al diccionario pueden aparecer estos archivos:

| Archivo | Para qué sirve |
|---|---|
| `.mdidx` | Índice de palabras para los formatos que no traen uno propio |
| `.mddat` | Babylon: el contenido descomprimido en trozos, para leer cada artículo directamente |
| `.mdzck` | EPUB grandes: puntos de lectura que evitan descomprimir el archivo entero en cada consulta |
| `.mdpos`, `.mdinf` | Mobipocket: posiciones de los artículos y formas flexionadas (*houses* → *house*, *went* → *go*) |

Si la carpeta del diccionario no admite escritura, se guardan en `koreader/cache/multidict/`. Se pueden borrar sin problema: el plugin los vuelve a crear cuando hacen falta.

### Estilos y arreglos del HTML

Funciona igual que con StarDict. Pon un `nombre.css` junto al diccionario para cambiar su aspecto, o un `nombre.lua` con una función que arregle el HTML antes de mostrarlo. Los `.css` que acompañan a los `.mdx` se usan solos.

---

## Detalles por formato

- **MDict:** se siguen las redirecciones `@@@LINK=` y los enlaces `entry://`. Las imágenes y el audio de los `.mdd` todavía no se muestran.
- **Mobipocket:** busca también por formas flexionadas si el diccionario las trae. Las imágenes del diccionario, como las de pronunciación, se muestran en el artículo.
- **EPUB:** sigue la especificación [EPUB 3 Dictionaries](https://idpf.org/epub/dict/epub-dict.html). Los EPUB que son libros normales se ignoran.
- **Kobo:** varios diccionarios del mismo idioma pueden convivir si tienen nombres distintos o están en subcarpetas distintas.
- **PocketBook:** se respeta la cursiva y la negrita de los diccionarios hechos con el conversor oficial.
- **Enlaces a un punto concreto:** un enlace como `bword://campaña#S1381…`, que lleva a una parte de otra definición, abre esa definición y salta a ese punto.

---

## Lo que no se puede leer

- Los diccionarios **comprados en la tienda de Kobo**: están cifrados. El plugin los detecta y lo avisa.
- Los `.mdx` que piden **registro**, es decir, un código o un correo para abrirse.
- Los Mobipocket/Kindle **con DRM**.
- Los PocketBook **cifrados**.
- En Android de 32 bits, los diccionarios de **más de 2 GB**.

## Limitaciones

- La búsqueda aproximada (*fuzzy*) busca palabras que empiezan igual. No corrige faltas de ortografía como hace StarDict.
- Las imágenes y sonidos de MDict (`.mdd`), DSL, Zim y Apple todavía no se muestran.
- TangoDict: las plantillas MUSTACHE no se aplican. El contenido se muestra tal cual.

---

## Problemas frecuentes

**No aparece un diccionario.**
Abre **Status of loaded dictionaries**: si el archivo se encontró pero no se pudo abrir, ahí sale el motivo. Comprueba también que está dentro de `koreader/data/dict/` y usa **Rescan dictionary folder**.

**"The native library could not be loaded".**
La librería no corresponde a tu aparato o falta la carpeta `libs/`. Vuelve a copiar el plugin completo. La misma ventana indica qué arquitectura ha detectado.

**Se queda mucho rato en "Indexing dictionary".**
Solo pasa la primera vez y en los formatos sin índice propio. Con un diccionario de cientos de MB en un lector lento, puede tardar unos minutos. Si se interrumpe, no pasa nada: la próxima vez empieza de nuevo.

**He cambiado el diccionario y sigue saliendo lo antiguo.**
Usa **Rescan dictionary folder**. Si aun así no cambia, borra los archivos `.md*` de ese diccionario.

---

## Cómo funciona por dentro

KOReader consulta los StarDict con el programa `sdcv`. Este plugin hace lo mismo con el resto de formatos gracias a una librería en C (`libmultidict.so`). La librería busca directamente en el índice que trae cada diccionario y lee del archivo solo la entrada pedida. Así gasta muy poca memoria, incluso con diccionarios de cientos de MB.

La forma de leer cada formato está basada en los lectores de [pyglossary](https://github.com/ilius/pyglossary).

Incluye código de terceros, todo con licencias libres:

- miniz, LZ4, xxHash, bzip2 y Zstandard (decodificador)
- LZMA SDK
- SQLite





-------------------------------------------------------------------------------------------------------




# Multi-format dictionaries (multidict) for KOReader

*Versión en español: [README.md](README.md)*

This plugin lets KOReader use dictionaries in almost any format, not just StarDict. MDict, Lingvo, Kindle, EPUB, Kobo, PocketBook, Babylon, Apple and many more are opened directly. You don't need to convert them first with pyglossary or any other tool.

The plugin's dictionaries show up next to your StarDict ones in the usual lookup window. In **Dictionary settings** you can reorder them, disable them or add them to presets, and their internal links work like in any other dictionary.

Version 1.5.0.

---

## Supported formats

| Format | File to put in the folder |
|---|---|
| MDict (1.2, 2.0 and 3.0) | `.mdx`, plus its `.css` if it has one |
| ABBYY Lingvo (DSL) | `.dsl` or `.dsl.dz` |
| Mobipocket / Kindle | `.mobi`, `.prc`, `.azw` |
| Dictionary EPUB (EPUB 3 Dictionaries, pyglossary EPUB 2, Kindle markup) | `.epub` |
| Kobo | `dicthtml-xx.zip` |
| PocketBook | `.dic` (SDIC) |
| Babylon | `.bgl` |
| Apple Dictionary | the whole `.dictionary` folder |
| TangoDict (DictTango) | `.dc` |
| mdict-utils SQLite | `.db` |
| dictd / dict.org | `.index` + `.dict` or `.dict.dz` |
| Aard 2 | `.slob` |
| Zim (Kiwix, e.g. Wiktionary) | `.zim` |
| QuickDic 6/7 | `.quickdic`, `.quickdic.v006.zip` |
| XDXF | `.xdxf` |
| StarDict textual | `.xml` |
| Text table | `.tab`, `.tsv`, `.csv` |
| Regular StarDict | `.ifo` + `.idx` + `.dict`: still read by KOReader as always |

---

## Installation

### Kobo, Kindle, PocketBook, reMarkable and Linux

1. Unzip the archive. You'll get a folder called `multidict.koplugin`.
2. Copy it into `koreader/plugins/`.
3. Put your dictionaries in KOReader's dictionary folder, `koreader/data/dict/`. You can use subfolders, for example one per dictionary.
4. Restart KOReader.

### Android

The steps are the same. The `koreader/` folder is on the device's internal storage, the same place where KOReader keeps its settings.

Android doesn't allow loading libraries from shared storage. So, on startup, the plugin copies its library to KOReader's private folder and loads it from there. You don't have to do anything.

It works on Android 4.3 and later, on ARM and x86 processors, both 32-bit and 64-bit.

### Which library does it use?

The plugin picks the right one for your device automatically. All of them are in `libs/`:

| Folder | Devices |
|---|---|
| `armhf` | Kobo, Kindle with firmware 5.16.3 or later, PocketBook, reMarkable 1 |
| `armel` | Kindle with older firmware |
| `arm64` | reMarkable 2 / Paper Pro, 64-bit ARM Linux |
| `x86_64` | KOReader on a Linux PC |
| `android-*` | Android (all four architectures) |

If you're short on space, you can delete the folders that don't match your device.

---

## Usage

Long-press a word while reading, as usual. Results from these dictionaries are mixed with the StarDict ones, in the order you chose in **Dictionary settings**.

**Tools → More tools → Multi-format dictionaries** has two options:

- **Status of loaded dictionaries:** shows which dictionaries were loaded and how many entries each one has. If one couldn't be opened, it shows why.
- **Rescan dictionary folder:** looks for dictionaries in the folder again. Use it to add or remove dictionaries without restarting.

### The first time you open some dictionaries

Most formats come with their own index and work instantly from the start. Some don't: DSL, XDXF, TSV/CSV, StarDict textual, Babylon, EPUB and Apple. For those, the first time you'll see *"Indexing dictionary (only the first time)"* while the plugin builds a small index. With a very large dictionary this can take a while. After that it doesn't happen again, unless the dictionary file changes.

### Files the plugin creates

These files may appear next to the dictionary:

| File | What it's for |
|---|---|
| `.mdidx` | Word index for formats that don't come with their own |
| `.mddat` | Babylon: the decompressed content in chunks, so each entry can be read directly |
| `.mdzck` | Large EPUBs: resume points that avoid decompressing the whole file on every lookup |
| `.mdpos`, `.mdinf` | Mobipocket: entry positions and inflected forms (*houses* → *house*, *went* → *go*) |

If the dictionary folder isn't writable, they are stored in `koreader/cache/multidict/`. They're safe to delete: the plugin recreates them when needed.

### Styles and HTML fixes

This works the same as with StarDict. Put a `name.css` next to the dictionary to change how it looks, or a `name.lua` with a function that fixes the HTML before it's shown. The `.css` files that come with `.mdx` dictionaries are used automatically.

---

## Format notes

- **MDict:** `@@@LINK=` redirects and `entry://` links are followed. Images and audio from `.mdd` files are not shown yet.
- **Mobipocket:** also finds inflected forms if the dictionary includes them. Images in the dictionary, such as pronunciation images, are shown in the entry.
- **EPUB:** follows the [EPUB 3 Dictionaries](https://idpf.org/epub/dict/epub-dict.html) specification. EPUBs that are regular books are ignored.
- **Kobo:** several dictionaries for the same language can coexist if they have different names or are in different subfolders.
- **PocketBook:** italics and bold are kept in dictionaries made with the official converter.
- **Links to a specific spot:** a link such as `bword://campaña#S1381…`, pointing to a part of another entry, opens that entry and jumps to that spot.

---

## What can't be read

- Dictionaries **bought from the Kobo store**: they're encrypted. The plugin detects them and tells you.
- `.mdx` files that require **registration**, i.e. a code or an email to open.
- Mobipocket/Kindle files **with DRM**.
- **Encrypted** PocketBook dictionaries.
- On 32-bit Android, dictionaries **larger than 2 GB**.

## Limitations

- Fuzzy search finds words that start the same way. It doesn't correct spelling mistakes the way StarDict does.
- Images and sounds from MDict (`.mdd`), DSL, Zim and Apple are not shown yet.
- TangoDict: MUSTACHE templates are not applied. The content is shown as is.

---

## Troubleshooting

**A dictionary doesn't show up.**
Open **Status of loaded dictionaries**: if the file was found but couldn't be opened, the reason is shown there. Also check that it's inside `koreader/data/dict/` and use **Rescan dictionary folder**.

**"The native library could not be loaded".**
The library doesn't match your device, or the `libs/` folder is missing. Copy the whole plugin again. The same window shows which architecture was detected.

**It stays on "Indexing dictionary" for a long time.**
This only happens the first time, and only with formats that have no index of their own. With a dictionary of hundreds of MB on a slow e-reader, it can take a few minutes. If it gets interrupted, nothing breaks: it starts over next time.

**I changed a dictionary and still get the old content.**
Use **Rescan dictionary folder**. If it still doesn't change, delete the `.md*` files for that dictionary.

---

## How it works

KOReader looks up StarDict dictionaries with the `sdcv` program. This plugin does the same for the other formats with a C library (`libmultidict.so`). The library searches directly in each dictionary's own index and reads only the requested entry from the file. This keeps memory use very low, even with dictionaries of hundreds of MB.

The way each format is read is based on the [pyglossary](https://github.com/ilius/pyglossary) readers. 

It includes third-party code, all under free licenses:

- miniz, LZ4, xxHash, bzip2 and Zstandard (decoder)
- LZMA SDK
- SQLite
