#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace fc {

// One emoji-capable font file found on this machine.
struct SystemEmojiFont {
    QString family; // the font's own family name (name table)
    QString path;   // absolute file path
    QString format; // "CBDT" or "sbix" (the bitmap tables it carries)
};

// The platform's font directories, in probe order (deterministic):
//   Windows: %WINDIR%\Fonts, %LOCALAPPDATA%\Microsoft\Windows\Fonts
//   macOS:   /System/Library/Fonts, /Library/Fonts, ~/Library/Fonts
//   others:  /usr/share/fonts, /usr/local/share/fonts,
//            ~/.local/share/fonts, ~/.fonts
QStringList systemFontDirectories();

// Scans the platform's font directories for emoji-capable fonts - the
// pool the Text panel's emoji-font picker lists, and where the
// color-emoji engine loads from (the app bundles no font of its own).
// A cheap table-directory probe (sfntBitmapEmojiFormat, the first
// ~64 KB of each file) rejects the fonts with no bitmap-emoji tables
// without parsing them; each candidate then gets a full parse
// (EmojiFont::load) plus a coverage check (U+1F600 must map to a
// glyph) so plain bitmap fonts never show up. Deterministic order:
// directories in systemFontDirectories() order, files sorted by path;
// a family name appearing in several files keeps the first file.
QVector<SystemEmojiFont> discoverEmojiFonts();

// The platform's preferred emoji font families, best first - the
// auto-pick order used until the user chooses one explicitly
// (Segoe UI Emoji on Windows, Apple Color Emoji on macOS, Noto on
// the Linux desktops).
QStringList preferredEmojiFontFamilies();

} // namespace fc
