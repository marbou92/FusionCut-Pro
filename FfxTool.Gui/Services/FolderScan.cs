using System;
using System.Collections.Generic;
using System.IO;

namespace FfxTool.Gui
{
    /// <summary>
    /// Shared folder-walking helpers for the pages that accept a whole
    /// folder of presets (Convert + Effect Lister).
    ///
    /// The walk is hand-rolled on purpose: .NET Framework's
    /// Directory.EnumerateFiles with AllDirectories throws MID-WALK on
    /// the first locked or denied subtree and loses every file after it.
    /// A per-directory catch means one unreadable subtree contributes
    /// nothing instead of killing the whole scan.
    /// </summary>
    internal static class FolderScan
    {
        /// <summary>Every *.ffx under dir (depth per the flag), sorted
        /// case-insensitively so a queue always reads in a stable order.</summary>
        public static List<string> Collect(string dir, bool recursive)
        {
            var found = new List<string>();
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
                Walk(dir, recursive, found);
            found.Sort(StringComparer.OrdinalIgnoreCase);
            return found;
        }

        public static void Walk(string dir, bool recursive, List<string> into)
        {
            try
            {
                foreach (var f in Directory.EnumerateFiles(dir, "*.ffx"))
                    into.Add(f);
                if (recursive)
                    foreach (var d in Directory.EnumerateDirectories(dir))
                        Walk(d, true, into);
            }
            catch { /* a locked or denied subtree just contributes nothing */ }
        }

        /// <summary>"1.2 MB" / "640.5 KB" / "512 B" for the folder report.</summary>
        public static string FmtSize(long bytes)
        {
            if (bytes >= 1024 * 1024) return ((double)bytes / (1024 * 1024)).ToString("0.#") + " MB";
            if (bytes >= 1024) return ((double)bytes / 1024).ToString("0.#") + " KB";
            return bytes + " B";
        }
    }
}
