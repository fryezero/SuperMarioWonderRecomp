// Applies a Switch update NCA (a BKTR patch) over its base NCA and extracts the merged RomFS, with LibHac (MIT,
// from NuGet). hactool stops at "Invalid BKTR layout!" on Wonder 1.2.1's patch and nstool cannot read it, so this
// is how the 1.2.1 RomFS was made (docs/nvn/WONDER.md, "Version 1.2.1"). Keys: your own prod.keys; the title keys
// file holds "<rights id> = <encrypted title key>" lines, both read from the tickets in the NSPs.
// usage: romfs_patch <prod.keys> <title.keys> <base.nca> <update.nca> <out dir>
using System;
using System.IO;
using LibHac.Common;
using IOPath = System.IO.Path;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;

static class P {
    static int Main(string[] a) {
        if (a.Length != 5) { Console.Error.WriteLine("usage: romfs_patch <prod.keys> <title.keys> <base.nca> <update.nca> <out dir>"); return 2; }
        KeySet ks = ExternalKeyReader.ReadKeyFile(a[0], a[1]);
        var baseNca = new Nca(ks, new LocalStorage(a[2], FileAccess.Read));
        var patchNca = new Nca(ks, new LocalStorage(a[3], FileAccess.Read));
        Console.WriteLine($"base  {baseNca.Header.TitleId:x16} sdk {baseNca.Header.SdkVersion}");
        Console.WriteLine($"patch {patchNca.Header.TitleId:x16} sdk {patchNca.Header.SdkVersion}");
        IStorage romfs = baseNca.OpenStorageWithPatch(patchNca, NcaSectionType.Data, IntegrityCheckLevel.None);
        romfs.GetSize(out long size).ThrowIfFailure();
        Console.WriteLine($"merged romfs: {size} bytes");
        var fs = new RomFsFileSystem(romfs);
        long files = 0, bytes = 0;
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories)) {
            if (e.Type != DirectoryEntryType.File) continue;
            string dst = IOPath.Combine(a[4], e.FullPath.TrimStart('/'));
            Directory.CreateDirectory(IOPath.GetDirectoryName(dst));
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            f.Get.GetSize(out long fsz).ThrowIfFailure();
            using var o = File.Create(dst);
            var buf = new byte[1 << 20];
            for (long off = 0; off < fsz;) {
                f.Get.Read(out long n, off, buf, ReadOption.None).ThrowIfFailure();
                if (n == 0) throw new Exception($"short read {e.FullPath}");
                o.Write(buf, 0, (int)n); off += n;
            }
            files++; bytes += fsz;
            if (files % 2000 == 0) Console.WriteLine($"  {files} files, {bytes >> 20} MiB");
        }
        Console.WriteLine($"done: {files} files, {bytes} bytes");
        return 0;
    }
}
