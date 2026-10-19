"""Prepares Qt 4.8.3 for building our own tool against the game's Qt, and checks it matches.

The game ships Qt 4.8.3's DLLs and no headers or import libraries. This takes the headers from the
official source package (third_party/qt-everywhere-opensource-src-4.8.3, see docs/tools), writes
the qconfig.h a Windows build of that version makes, and makes import libraries for x64 from the
game's own QtCore4.dll and QtGui4.dll, so ordinary Qt code compiles and links against them.

It then checks that the headers describe the game's Qt: for every class we may subclass, the number
of virtual function slots the compiler lays out from the headers (MSVC's
/d1reportSingleClassLayout) must equal the number in the vtable the game's DLL exports. A feature
Valve built off, or a change of theirs to Qt, would show as a different count, and a class of ours
deriving from it would call the wrong functions.

    python tools/qt4_sdk.py          prepare (idempotent) and check
"""
import os
import re
import struct
import subprocess
import sys

import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QT = os.path.join(ROOT, "third_party", "qt-everywhere-opensource-src-4.8.3")
OUT = os.path.join(ROOT, "third_party", "qt4")
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life Alyx\game\bin\win64"
VCVARS = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

# What configure.exe writes for a shared, open source Windows build of 4.8 with every feature on
# (tools/configure/configureapp.cpp, generateConfigfiles: QT_DLL when shared, the byte order, and
# QT_ARCH_ followed by its ARCHITECTURE, "windows" by default). A feature turned off would add a
# QT_NO_ line; the layout check below is what says none is needed.
QCONFIG = """/* Everything */

#ifndef QT_DLL
#define QT_DLL
#endif

/* License information */
#define QT_PRODUCT_LICENSEE "Open Source"
#define QT_PRODUCT_LICENSE "OpenSource"

// Qt Edition
#ifndef QT_EDITION
#  define QT_EDITION QT_EDITION_OPENSOURCE
#endif

/* Machine byte-order */
#define Q_BIG_ENDIAN 4321
#define Q_LITTLE_ENDIAN 1234
#define Q_BYTE_ORDER Q_LITTLE_ENDIAN

// Compile time features
#define QT_ARCH_WINDOWS
"""

# The classes a tool of ours may derive from or hold, and their headers.
CLASSES = ["QObject", "QAbstractItemModel", "QAbstractListModel", "QTimer", "QWidget", "QAction",
           "QMainWindow", "QDialog", "QFrame", "QLabel", "QAbstractButton", "QPushButton",
           "QToolButton", "QCheckBox", "QAbstractSlider", "QSlider", "QAbstractSpinBox",
           "QDoubleSpinBox", "QSpinBox", "QLineEdit", "QComboBox", "QAbstractScrollArea",
           "QScrollArea", "QDockWidget", "QSplitter", "QMenu", "QMenuBar", "QToolBar",
           "QStatusBar", "QAbstractItemView", "QListView", "QListWidget", "QTreeView",
           "QStyledItemDelegate", "QFileDialog", "QColorDialog", "QLayout", "QBoxLayout",
           "QFormLayout", "QGridLayout", "QGroupBox", "QTabWidget", "QStackedWidget"]


def run(cmd, cwd=None):
    r = subprocess.run('call "%s" >nul && %s' % (VCVARS, cmd), shell=True, capture_output=True,
                       text=True, cwd=cwd, errors="replace")
    return r.returncode, r.stdout + r.stderr


def exports(dll):
    pe = pefile.PE(os.path.join(GAME, dll), fast_load=True, max_symbol_exports=200000)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    return pe, [(e.name.decode(), e.address) for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name]


def qconfig_cpp():
    """What configure.exe writes as qconfig.cpp (generateConfigfiles): the licence and the install
    paths, which only moc's own build reads. The paths name our third_party/qt4."""
    base = OUT.replace("\\", "/")
    paths = [("prefix_path", "prfxpath", base), ("documentation_path", "docspath", base + "/doc"),
             ("headers_path", "hdrspath", base + "/include"), ("libraries_path", "libspath", base + "/lib"),
             ("binaries_path", "binspath", base + "/bin"), ("plugins_path", "plugpath", base + "/plugins"),
             ("imports_path", "impspath", base + "/imports"), ("data_path", "datapath", base),
             ("translations_path", "trnspath", base + "/translations"),
             ("examples_path", "xmplpath", base + "/examples"), ("demos_path", "demopath", base + "/demos")]
    s = ['/* Licensed */',
         'static const char qt_configure_licensee_str          [512 + 12] = "qt_lcnsuser=Open Source";',
         'static const char qt_configure_licensed_products_str [512 + 12] = "qt_lcnsprod=OpenSource";',
         '', '/* Build date */',
         'static const char qt_configure_installation          [11  + 12] = "qt_instdate=2012-09-13";', '']
    for name, tag, p in paths:
        s.append('static const char qt_configure_%s_str [512 + 12] = "qt_%s=%s";' % (name, tag, p))
    s.append('/* strlen( "qt_lcnsxxxx") == 12 */')
    s.append('#define QT_CONFIGURE_LICENSEE qt_configure_licensee_str + 12;')
    s.append('#define QT_CONFIGURE_LICENSED_PRODUCTS qt_configure_licensed_products_str + 12;')
    for name, _, _ in paths:
        s.append('#define QT_CONFIGURE_%s qt_configure_%s_str + 12;' % (name.upper(), name))
    return "\n".join(s) + "\n"


def prepare():
    os.makedirs(os.path.join(OUT, "lib"), exist_ok=True)
    qc = os.path.join(QT, "src", "corelib", "global", "qconfig.h")
    if not os.path.exists(qc) or open(qc).read() != QCONFIG:
        open(qc, "w", newline="\n").write(QCONFIG)
    qcc = os.path.join(QT, "src", "corelib", "global", "qconfig.cpp")
    if not os.path.exists(qcc) or open(qcc).read() != qconfig_cpp():
        open(qcc, "w", newline="\n").write(qconfig_cpp())
    for dll in ("QtCore4.dll", "QtGui4.dll"):
        stem = dll[:-4]
        lib = os.path.join(OUT, "lib", stem + ".lib")
        if os.path.exists(lib):
            continue
        _, names = exports(dll)
        deffile = os.path.join(OUT, "lib", stem + ".def")
        with open(deffile, "w") as f:
            f.write("LIBRARY %s\nEXPORTS\n" % dll)
            for n, _ in names:
                # Data symbols (vtables, static members) are imported as DATA.
                data = n.startswith("??_7") or re.match(r"^\?\w+@\w+@@2", n) is not None
                f.write("    %s%s\n" % (n, " DATA" if data else ""))
        code, out = run('lib /nologo /def:"%s" /machine:x64 /out:"%s"' % (deffile, lib))
        if code != 0:
            sys.exit("lib failed for %s:\n%s" % (dll, out[-2000:]))
    print("prepared: qconfig.h, %s" % os.path.join(OUT, "lib"))


# moc, built from the same source the way src/tools/moc/moc.pro and src/tools/bootstrap/bootstrap.pro
# build it: the bootstrap subset of QtCore and QtXml with zlib, compiled static, and moc's own files.
BOOTSTRAP_DEFINES = ["QT_BOOTSTRAPPED", "QT_LITE_UNICODE", "QT_TEXTCODEC", "QT_NO_CAST_FROM_ASCII",
                     "QT_NO_CAST_TO_ASCII", "QT_NO_CODECS", "QT_NO_DATASTREAM", "QT_NO_GEOM_VARIANT",
                     "QT_NO_LIBRARY", "QT_NO_QOBJECT", "QT_NO_STL", "QT_NO_SYSTEMLOCALE",
                     "QT_NO_TEXTSTREAM", "QT_NO_THREAD", "QT_NO_UNICODETABLES",
                     "QT_NO_USING_NAMESPACE", "QT_NO_DEPRECATED", "QT_NODLL", "QT_MOC"]
BOOTSTRAP_SOURCES = """
corelib/codecs/qisciicodec.cpp corelib/codecs/qlatincodec.cpp corelib/codecs/qsimplecodec.cpp
corelib/codecs/qtextcodec.cpp corelib/codecs/qtsciicodec.cpp corelib/codecs/qutfcodec.cpp
corelib/global/qglobal.cpp corelib/global/qmalloc.cpp corelib/global/qnumeric.cpp
corelib/io/qabstractfileengine.cpp corelib/io/qbuffer.cpp corelib/io/qdatastream.cpp corelib/io/qdir.cpp
corelib/io/qdiriterator.cpp corelib/io/qfile.cpp corelib/io/qfileinfo.cpp corelib/io/qfilesystementry.cpp
corelib/io/qfilesystemengine.cpp corelib/io/qfsfileengine.cpp corelib/io/qfsfileengine_iterator.cpp
corelib/io/qiodevice.cpp corelib/io/qtemporaryfile.cpp corelib/io/qtextstream.cpp
corelib/kernel/qmetatype.cpp corelib/kernel/qvariant.cpp corelib/kernel/qsystemerror.cpp
corelib/tools/qbitarray.cpp corelib/tools/qbytearray.cpp corelib/tools/qbytearraymatcher.cpp
corelib/tools/qdatetime.cpp corelib/tools/qhash.cpp corelib/tools/qlist.cpp corelib/tools/qlocale.cpp
corelib/tools/qlocale_tools.cpp corelib/tools/qmap.cpp corelib/tools/qregexp.cpp corelib/tools/qstring.cpp
corelib/tools/qstringlist.cpp corelib/tools/qvector.cpp corelib/tools/qvsnprintf.cpp
corelib/xml/qxmlutils.cpp corelib/xml/qxmlstream.cpp xml/dom/qdom.cpp xml/sax/qxml.cpp
corelib/io/qfilesystemengine_win.cpp corelib/io/qfilesystemiterator_win.cpp corelib/io/qfsfileengine_win.cpp
corelib/plugin/qsystemlibrary.cpp corelib/tools/qlocale_win.cpp
tools/moc/moc.cpp tools/moc/preprocessor.cpp tools/moc/generator.cpp tools/moc/parser.cpp
tools/moc/token.cpp tools/moc/main.cpp
""".split()
ZLIB_SOURCES = ["adler32.c", "compress.c", "crc32.c", "deflate.c", "gzclose.c", "gzlib.c", "gzread.c",
                "gzwrite.c", "infback.c", "inffast.c", "inflate.c", "inftrees.c", "trees.c", "uncompr.c",
                "zutil.c"]


def build_moc():
    exe = os.path.join(OUT, "bin", "moc.exe")
    if os.path.exists(exe):
        return exe
    objdir = os.path.join(OUT, "moc_obj")
    os.makedirs(objdir, exist_ok=True)
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    src = os.path.join(QT, "src")
    inc = " ".join('/I"%s"' % p for p in (
        os.path.join(QT, "include"), os.path.join(QT, "include", "QtCore"),
        os.path.join(QT, "include", "QtXml"), os.path.join(src, "xml"),
        os.path.join(src, "3rdparty", "zlib"), os.path.join(src, "tools", "moc"),
        os.path.join(QT, "mkspecs", "win32-msvc2010")))
    defs = " ".join("/D%s" % d for d in BOOTSTRAP_DEFINES)
    files = " ".join('"%s"' % os.path.join(src, f) for f in BOOTSTRAP_SOURCES)
    zfiles = " ".join('"%s"' % os.path.join(src, "3rdparty", "zlib", f) for f in ZLIB_SOURCES)
    code, out = run('cl /nologo /c /O1 /EHsc /MT /Zc:wchar_t- /w /DUNICODE /D_CRT_SECURE_NO_WARNINGS '
                    '%s %s %s' % (defs, inc, files), cwd=objdir)
    if code != 0:
        errs = [l for l in out.splitlines() if "error" in l]
        print("\n".join(errs[:40]))
        sys.exit("moc's sources did not compile")
    code, out = run('cl /nologo /c /O1 /MT /w %s %s' % (inc, zfiles), cwd=objdir)
    if code != 0:
        print(out[-3000:])
        sys.exit("zlib did not compile")
    code, out = run('link /nologo /OUT:"%s" *.obj user32.lib advapi32.lib ole32.lib shell32.lib' % exe,
                    cwd=objdir)
    if code != 0:
        print(out[-3000:])
        sys.exit("moc did not link")
    print("built", exe)
    return exe


def dll_vtable_slots():
    """Slots in each exported vtable of the game's Qt: code pointers up to the next table's RTTI."""
    slots = {}
    for dll in ("QtCore4.dll", "QtGui4.dll"):
        pe, names = exports(dll)
        img = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        text = [s for s in pe.sections if s.Characteristics & 0x20000000]

        def code(v):
            r = v - base
            return any(s.VirtualAddress <= r < s.VirtualAddress + s.Misc_VirtualSize for s in text)
        for n, rva in names:
            m = re.match(r"^\?\?_7(\w+)@@6B(?:(\w+)@@)?@$", n)
            if not m:
                continue
            k = 0
            while code(struct.unpack_from("<Q", img, rva + 8 * k)[0]):
                k += 1
            slots[(m.group(1), m.group(2) or m.group(1))] = k
    return slots


def header_vtable_slots():
    """The same counts as the compiler lays them out from the headers."""
    src = os.path.join(OUT, "layout_check.cpp")
    with open(src, "w") as f:
        f.write("#include <QtCore/QtCore>\n#include <QtGui/QtGui>\n")
        for c in CLASSES:
            f.write("struct Probe_%s : %s { virtual ~Probe_%s(); };\nProbe_%s::~Probe_%s() {}\n"
                    % (c, c, c, c, c))
    # Every class's layout: the compiler honours only the last /d1reportSingleClassLayout given.
    flags = "/d1reportAllClassLayout"
    inc = '/I"%s" /I"%s" /I"%s"' % (os.path.join(QT, "include"), os.path.join(QT, "include", "QtCore"),
                                    os.path.join(QT, "include", "QtGui"))
    code, out = run('cl /nologo /c /EHsc /Zc:wchar_t- /DUNICODE /DQT_NO_DEBUG /DQT_CORE_LIB /DQT_GUI_LIB '
                    '/DQT_DLL %s %s /Fo"%s" "%s"' % (inc, flags, os.path.join(OUT, "layout_check.obj"), src),
                    cwd=OUT)
    if code != 0:
        print(out[-4000:])
        sys.exit("the layout probe did not compile")
    # Each report starts "class X size(...)" and lists "X::$vftable@Base@:" tables, each followed
    # by numbered entries " | <n>\t| &...".
    res = {}
    cls = None
    table = None
    for line in out.splitlines():
        m = re.match(r"^class (\w+)\s+size\(", line)
        if m:
            cls = m.group(1)
            table = None
            continue
        m = re.match(r"^(\w+)::\$vftable@(\w*)@?:", line)
        if m and cls and m.group(1) == cls:
            table = m.group(2) or cls
            res[(cls, table)] = 0
            continue
        m = re.match(r"^\s*\|?\s*(\d+)\s*\|\s*&", line)
        if m and table is not None:
            res[(cls, table)] = max(res[(cls, table)], int(m.group(1)) + 1)
    return res


def check():
    dll = dll_vtable_slots()
    hdr = header_vtable_slots()
    bad = 0
    for c in CLASSES:
        rows = [(k, v) for k, v in hdr.items() if k[0] == c]
        if not rows:
            print("%-22s no layout reported" % c)
            bad += 1
            continue
        for (cls, base), n in rows:
            key = (cls, cls if base in ("", cls) else base)
            want = dll.get(key, dll.get((cls, cls)))
            ok = want == n
            bad += not ok
            print("%-22s %-14s headers %3d slots, game's DLL %s%s" % (
                cls, "(" + base + ")" if base and base != cls else "", n, want, "" if ok else "   <-- DIFFERENT"))
    print("ALL MATCH" if bad == 0 else "%d DIFFER" % bad)
    return bad == 0


if __name__ == "__main__":
    prepare()
    build_moc()
    sys.exit(0 if check() else 1)
