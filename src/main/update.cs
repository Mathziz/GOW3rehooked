// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

internal static class Update {
    const string Repository = "cuesta4/GOW3rehooked";
    const int Handled = 10;
    static readonly string App = AppDomain.CurrentDomain.BaseDirectory;
    const string Cache = "_rehooked_/shader_cache/recipes-hash2-aes.bin";
    const string Runtime = "gow3.dll";
    static readonly string[] Programs = { "gow3_debug.exe", "gow3_update.exe", "gow3.exe", Runtime };
    static readonly Regex Tag = new Regex(@"^v(\d+)\.(\d+)(?:\.(\d+))?(?:-(alpha|beta|rc)(?:\.(\d+))?)?$");

    class Asset { public string name { get; set; } public string digest { get; set; } public long size { get; set; } }
    class Release { public string tag_name { get; set; } public Asset[] assets { get; set; } }

    static int[] VersionParts(string tag) {
        var match = Tag.Match(tag);
        if (!match.Success) throw new FormatException("Unknown release tag.");
        var g = match.Groups;
        return new[] { Number(g[1]), Number(g[2]), Number(g[3]),
            Array.IndexOf(new[] { "alpha", "beta", "rc", "" }, g[4].Value), Number(g[5]) };
    }

    static int Number(Group group) { return group.Success ? int.Parse(group.Value) : 0; }

    static string Hash(byte[] bytes) {
        using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
    }

    // Let Attachment Services apply the machine's download policy and record the origin.
    [ComImport, Guid("73DB1241-1E85-4581-8E4F-A81E1D0F8C57"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAttachmentExecute {
        void SetClientTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetClientGuid(ref Guid guid);
        void SetLocalPath([MarshalAs(UnmanagedType.LPWStr)] string path);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void SetSource([MarshalAs(UnmanagedType.LPWStr)] string source);
        void SetReferrer([MarshalAs(UnmanagedType.LPWStr)] string url);
        void CheckPolicy();
        void Prompt(IntPtr owner, int prompt, out int action);
        void Save();
    }

    static void RegisterDownload(string path, string url) {
        var attachment = (IAttachmentExecute)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("4125DD96-E03A-4103-8F70-E0597D803B9C")));
        try { attachment.SetLocalPath(path); attachment.SetSource(url); attachment.Save(); }
        finally { Marshal.FinalReleaseComObject(attachment); }
    }

    sealed class InstallFile {
        public string Source, Target, Backup;
        public bool Moved, Installed;
    }

    static void CheckDirectory(string directory) {
        // Destination subfolders may already belong to the player.
        if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Update destination is a link: " + directory);
    }

    static void Install(byte[] bytes, string url, Action relaunch) {
        string stage = Path.Combine(App, ".gow3-update-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(stage);
        var files = new List<InstallFile>();
        bool cleanup = true;
        try {
            using (var archive = new ZipArchive(new MemoryStream(bytes), ZipArchiveMode.Read)) {
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                long total = 0;
                foreach (var entry in archive.Entries) {
                    string name = entry.FullName.Replace('\\', '/');
                    if (name.EndsWith("/")) continue;
                    bool program = Programs.Contains(name);
                    if (!program && name != Cache && !Regex.IsMatch(name, @"^licenses/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.txt$"))
                        throw new InvalidDataException("Unexpected file in release: " + name);
                    if (!names.Add(name) || entry.Length <= 0 || entry.Length > (256L << 20) - total)
                        throw new InvalidDataException("Invalid release contents.");
                    total += entry.Length;
                    var target = Path.Combine(App, name.Replace('/', '\\'));
                    // Explorer shortcuts keep pointing at the original, user-approved launchers.
                    if ((name == Cache || name == "gow3.exe" || name == "gow3_debug.exe") && File.Exists(target)) continue;
                    var directory = App;
                    var parts = name.Split('/');
                    for (int i = 0; i < parts.Length - 1; ++i) {
                        directory = Path.Combine(directory, parts[i]);
                        Directory.CreateDirectory(directory);
                        CheckDirectory(directory);
                    }
                    string source = Path.Combine(stage, Path.GetFileName(name));
                    using (var output = new FileStream(source, FileMode.CreateNew))
                    using (var input = entry.Open()) input.CopyTo(output);
                    if (program) RegisterDownload(source, url);
                    files.Add(new InstallFile { Source = source, Target = target, Backup = source + ".old" });
                }
                if (!Programs.All(names.Contains)) throw new InvalidDataException("Release is missing a required program file.");
            }
            files = files.OrderBy(f => Path.GetFileName(f.Target) == Runtime).ToList();
            foreach (var file in files) {
                if (File.Exists(file.Target)) {
                    // Windows can keep both running images mapped until our caller exits.
                    string name = Path.GetFileName(file.Target);
                    if (name == Runtime || name == "gow3_update.exe") {
                        file.Backup = Path.Combine(App, Path.GetFileNameWithoutExtension(name) + ".update.old");
                        File.Delete(file.Backup);
                    }
                    File.Move(file.Target, file.Backup);
                    file.Moved = true;
                }
                File.Move(file.Source, file.Target);
                file.Installed = true;
            }
            relaunch();
        } catch {
            var failures = new List<string>();
            foreach (var file in Enumerable.Reverse(files)) {
                try {
                    if (file.Installed) File.Delete(file.Target);
                    if (file.Moved) File.Move(file.Backup, file.Target);
                } catch (IOException e) { failures.Add(e.Message); }
                  catch (UnauthorizedAccessException e) { failures.Add(e.Message); }
            }
            if (failures.Count != 0) {
                cleanup = false;
                throw new IOException("Could not restore every file. Backups: " + stage + "\n" + String.Join("\n", failures));
            }
            throw;
        } finally {
            if (cleanup) {
                try { Directory.Delete(stage, true); }
                catch (IOException) {}
                catch (UnauthorizedAccessException) {}
            }
        }
    }

    static int Run(string[] args) {
        var current = VersionParts(args[0]);
        string name = "GOW3rehooked-Windows-x86_64" + (args[1] == "AVX1" ? "-AVX1" : "") + ".zip";
        string state = Path.Combine(args[2], "_rehooked_", "settings", "skipped-release.txt");
        Release release;
        using (var client = new HttpClient { Timeout = TimeSpan.FromSeconds(3), MaxResponseContentBufferSize = 1 << 20 }) {
            client.DefaultRequestHeaders.UserAgent.ParseAdd("GOW3rehooked-updater/1");
            string json = client.GetStringAsync("https://api.github.com/repos/" + Repository + "/releases/latest").GetAwaiter().GetResult();
            release = new JavaScriptSerializer().Deserialize<Release>(json);
        }
        string tag = release.tag_name;
        int cmp = System.Collections.StructuralComparisons.StructuralComparer.Compare(VersionParts(tag), current);
        if (cmp <= 0 || (File.Exists(state) && File.ReadAllText(state) == tag)) return 0;
        var asset = release.assets.Single(a => a.name == name);
        if (asset.size <= 0 || asset.size > (128 << 20) || !Regex.IsMatch(asset.digest, "^sha256:[a-f0-9]{64}$")) return 0;
        using (var offer = Process.Start(new ProcessStartInfo(Path.Combine(App, "gow3.exe"), "--update-offer " + tag) {
            UseShellExecute = false, RedirectStandardInput = true, RedirectStandardOutput = true
        })) {
            int answer = offer.StandardOutput.BaseStream.ReadByte();
            if (answer < 0) {
                offer.WaitForExit();
                return offer.ExitCode == 0 ? Handled : 0;
            }
            try {
                if (answer == 'N') {
                    Directory.CreateDirectory(Path.GetDirectoryName(state));
                    File.WriteAllText(state, tag);
                    return 0;
                }
                if (answer != 'Y') return Handled;
                string url = "https://github.com/" + Repository + "/releases/download/" + tag + "/" + name;
                using (var client = new HttpClient { Timeout = TimeSpan.FromMinutes(3), MaxResponseContentBufferSize = asset.size }) {
                    var bytes = client.GetByteArrayAsync(url).GetAwaiter().GetResult();
                    if (bytes.LongLength != asset.size || "sha256:" + Hash(bytes) != asset.digest)
                        throw new InvalidDataException("The ZIP does not match the release checksum.");
                    Install(bytes, url, delegate {
                        offer.StandardInput.Close();
                        offer.WaitForExit();
                        var start = new ProcessStartInfo(Path.Combine(App, "gow3.exe"), "--game-dir \"" + Path.Combine(args[2], ".") + "\"");
                        start.UseShellExecute = false;
                        using (Process.Start(start)) {}
                    });
                }
                return Handled;
            } catch (Exception error) {
                offer.StandardInput.Close();
                MessageBox.Show(error.Message, "GOW3rehooked update", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 0;
            } finally { offer.StandardInput.Close(); }
        }
    }

    [STAThread]
    static int Main(string[] args) {
        if (args.Length != 3) return 0;
        try {
            System.Net.ServicePointManager.SecurityProtocol = System.Net.SecurityProtocolType.Tls12;
            string key = Hash(Encoding.UTF8.GetBytes(App.ToUpperInvariant()));
            using (var mutex = new Mutex(false, "Local\\GOW3rehooked-update-" + key)) {
                try { mutex.WaitOne(); } catch (AbandonedMutexException) {}
                try { return Run(args); } finally { mutex.ReleaseMutex(); }
            }
        } catch (Exception) { return 0; } // Discovery is optional when GitHub is unavailable.
    }
}
