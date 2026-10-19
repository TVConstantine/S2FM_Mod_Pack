// The panel's two looks. Its colours are written in a purple theme; when S2FM runs in Valve's own
// palette (game/core/tools/stylesheets/qtabbedstyle_colors.vsc as the 2022 build ships it: Window #363636, Base
// #262627, Button #494949, Text #b6b6b7, Highlight #4f5259 ...), each is drawn as the grey of that palette for the
// same part instead, and the purple accent as a lighter Highlight. Which one is decided from the palette S2FM gives
// Qt: neutral greys are Valve's, anything else is a theme (purple). [GENERAL] Theme=purple|s2fm in
// s2fm_reshade/state.ini overrides it.
#pragma once

#include <QtGui/QColor>
#include <QtCore/QRegExp>
#include <QtCore/QString>

// True: Valve's greys.
inline bool& RsThemeStock()
{
    static bool stock = false;
    return stock;
}

// The colour to draw a purple one with.
inline QRgb RsThemeRgb(QRgb purple)
{
    if (!RsThemeStock()) return purple;
    struct Pair { QRgb from, to; };
    static const Pair kMap[] = {
        // Surfaces, darkest first: the side and the status bar, the page, the header, the cards, inputs.
        { 0x0c0a13, 0x2a2a2a }, { 0x120f1c, 0x303030 }, { 0x141020, 0x363636 }, { 0x151122, 0x363636 },
        { 0x191426, 0x2e2e2e }, { 0x1a1526, 0x363636 }, { 0x1e182c, 0x262627 }, { 0x211a33, 0x414141 },
        { 0x221b33, 0x494949 }, { 0x2b2040, 0x4f5259 }, { 0x2e2342, 0x494949 }, { 0x3b2361, 0x4f5259 },
        { 0x4c2882, 0x5f646d },
        // Lines and handles.
        { 0x2b213f, 0x262627 }, { 0x3b2d54, 0x4a4a4a }, { 0x483466, 0x5b5b5b }, { 0x36294d, 0x5b5b5b },
        { 0x63478c, 0x6e6e6e }, { 0x3d216b, 0x4f5259 },
        // Text, brightest first.
        { 0xede9fe, 0xc8c8c8 }, { 0xe9d5ff, 0xe6e6e6 }, { 0xc4b5fd, 0xd0d0d0 }, { 0xa79fc4, 0xa0a0a1 },
        { 0x9d93be, 0x989899 }, { 0x7a7095, 0x8a8a8b }, { 0x6f658c, 0x8a8a8b }, { 0x63587c, 0x6e6e6e },
        { 0x5a5075, 0x757575 },
        // The accent: S2FM's Highlight, lighter.
        { 0xa855f7, 0x8c9bb0 }, { 0xc084fc, 0xb9c2cf }, { 0x7c3aed, 0x6f7f96 },
    };
    const QRgb rgb = purple & 0xffffff;
    for (const Pair& p : kMap)
        if (p.from == rgb) return (purple & 0xff000000) | p.to;
    return purple;
}

inline QColor RsC(int r, int g, int b, int a = 255)
{
    const QRgb c = RsThemeRgb(qRgba(r, g, b, a));
    return QColor(qRed(c), qGreen(c), qBlue(c), a);
}

// A style sheet written in the purple, its #rrggbb colours put through the map.
inline QString RsThemeSheet(const QString& purple)
{
    if (!RsThemeStock()) return purple;
    QString out;
    QRegExp hex(QString::fromLatin1("#([0-9a-fA-F]{6})\\b"));
    int at = 0, from = 0;
    while ((at = hex.indexIn(purple, from)) >= 0) {
        out += purple.mid(from, at - from);
        const QRgb c = RsThemeRgb(hex.cap(1).toUInt(0, 16));
        out += QString::fromLatin1("#%1").arg(c & 0xffffff, 6, 16, QChar('0'));
        from = at + hex.matchedLength();
    }
    return out + purple.mid(from);
}
