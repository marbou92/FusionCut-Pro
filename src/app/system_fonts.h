#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace fc {

// One emoji-capable font file found on this machine.
struct SystemEmojiFont {
    QString family; // the font's own family name (name table)
    QString path;   // absolute file path
    QString format; // "CBDT" / "sbix" (color bitmaps) or "outline"
                    // (emoji glyphs with no color tables - Segoe UI
                    // Symbol, Symbola, Noto Emoji...)
};

// The platform's font directories, in probe order (deterministic):
//   Windows: %WINDIR%\Fonts, %LOCALAPPDATA%\Microsoft\Windows\Fonts
//   macOS:   /System/Library/Fonts, /Library/Fonts, ~/Library/Fonts
//   others:  /usr/share/fonts, /usr/local/share/fonts,
//            ~/.local/share/fonts, ~/.fonts
QStringList systemFontDirectories();

// Scans the machine for emoji-capable fonts - the pool the Text panel's
// emoji-font picker lists, and where the color-emoji engine loads from
// (the app bundles no font of its own).
//
// WHAT COUNTS AS EMOJI-CAPABLE: a font must map a representative
// battery of emoji codepoints (emoji_clusters.h) to glyphs. Faces that
// carry CBDT/CBLC or sbix bitmap tables (Segoe UI Emoji, Apple Color
// Emoji, Noto Color Emoji...) must ALSO have real bitmap records
// behind the battery - a tagged-but-empty face does not qualify; they
// list as "CBDT"/"sbix". Faces WITHOUT bitmap tables qualify as
// "outline" (Segoe UI Symbol, Symbola, Noto Emoji): picking one routes
// emoji clusters through that family on the platform text stack.
//
// WHERE IT LOOKS: the platform font directories (recursively) PLUS, on
// Windows, the machine and user font registrations
// (HKLM/HKCU\Software\Microsoft\Windows NT\CurrentVersion\Fonts) so
// fonts installed outside the standard directories are found too.
//
// NO REPEATS: entries are deduplicated three ways - by canonical file
// path, by NORMALIZED family key (case, spacing, and a trailing
// "regular" folded away, so the same family installed twice or spelled
// slightly differently lists once), and by content signature (file
// size + first/last 4 KB hashes, so a renamed byte-identical copy of
// an already-listed font cannot appear twice).
//
// Deterministic order: directories in systemFontDirectories() order,
// files sorted by path, registry-only files after those (sorted).
QVector<SystemEmojiFont> discoverEmojiFonts();

// The platform's preferred emoji font families, best first - the
// auto-pick order used until the user chooses one explicitly
// (Segoe UI Emoji on Windows, Apple Color Emoji on macOS, Noto on
// the Linux desktops). All color faces.
QStringList preferredEmojiFontFamilies();

} // namespace fc
