using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using X360.Other;
using X360.STFS;

namespace PPLoaderPkgBuilder
{
    internal static class Program
    {
        private const string ContainerName = "FFFF505000000001";

        private static int Main(string[] args)
        {
            try
            {
                string root = Path.GetFullPath(args.Length > 0
                    ? args[0]
                    : Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "..", ".."));
                Build(root);
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("PPLoader container build failed: " + ex.Message);
                return 1;
            }
        }

        private static void Build(string root)
        {
            string build = Path.Combine(root, "Build");
            string stage = Path.Combine(build, "PPLoaderPackage");
            Directory.CreateDirectory(stage);

            string rawInstaller = Path.Combine(build, "PPLoaderInstaller", "default.xex");
            string stagedInstaller = Path.Combine(stage, "default.xex");
            string loader = Path.Combine(build, "pploader", "pploader.xex");
            string xbdm = Path.Combine(root, "installer", "payloads", "xbdm.xex");
            string font = Path.Combine(root, "installer", "assets", "Arial_12.xpr");
            string icon = Path.Combine(root, "installer", "assets", "XContentThumbnail.png");

            RequireFile(rawInstaller);
            RequireFile(loader);
            RequireFile(xbdm);
            RequireFile(font);
            RequireFile(icon);

            ConvertInstallerXex(root, rawInstaller, stagedInstaller);
            File.Copy(loader, Path.Combine(stage, "pploader.xex"), true);
            File.Copy(xbdm, Path.Combine(stage, "xbdm.xex"), true);
            File.Copy(font, Path.Combine(stage, "Arial_12.xpr"), true);
            File.Copy(icon, Path.Combine(stage, "XContentThumbnail.png"), true);

            var sources = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                { "pploader.xex", Path.Combine(stage, "pploader.xex") },
                { "xbdm.xex", Path.Combine(stage, "xbdm.xex") },
                { "default.xex", stagedInstaller },
                { "Arial_12.xpr", Path.Combine(stage, "Arial_12.xpr") },
                { "XContentThumbnail.png", Path.Combine(stage, "XContentThumbnail.png") }
            };

            string output = Path.Combine(build, ContainerName);
            if (File.Exists(output))
                File.Delete(output);

            var create = new CreateSTFS();
            create.STFSType = STFSType.Type0;
            HeaderData header = create.HeaderData;
            header.ThisType = PackageType.GameDemo;
            header.TitleID = 0xFFFF5050;
            header.MediaID = 0x50504C44;
            header.Version_ = 0x10000000;
            header.Version_Base = 0x10000000;
            header.IDTransfer = TransferLock.AllowTransfer;
            header.SetLanguage(Languages.English);
            header.Title_Display = "PPLoader Installer";
            header.Title_Package = "PPLoader Installer";
            header.Description = "Install, repair, update, or remove the PeerPressure plugin-loader patch.";
            header.Publisher = "TooMuchPressure";

            byte[] iconBytes = File.ReadAllBytes(icon);
            if (iconBytes.Length > 0x4000)
                throw new InvalidDataException("Container icon exceeds the 16 KiB STFS limit.");
            header.PackageImageBinary = iconBytes;
            header.ContentImageBinary = iconBytes;

            foreach (KeyValuePair<string, string> source in sources)
            {
                if (!create.AddFile(source.Value, source.Key))
                    throw new InvalidOperationException("Could not add " + source.Key + " to the package.");
            }

            var rsa = new RSAParams(StrongSigned.LIVE);
            var log = new LogRecord();
            var package = new STFSPackage(create, rsa, output, log);
            package.CloseIO();
            RequireFile(output);

            VerifyPackage(output, sources, Path.Combine(build, "VerifiedPayloads"));

            string contentOutput = Path.Combine(build, "Content", "0000000000000000",
                "FFFF5050", "00080000", ContainerName);
            Directory.CreateDirectory(Path.GetDirectoryName(contentOutput));
            File.Copy(output, contentOutput, true);
            if (!FilesEqual(output, contentOutput))
                throw new InvalidDataException("Content-tree copy did not match the verified container.");

            Console.WriteLine("Built and verified: " + contentOutput);
        }

        private static void ConvertInstallerXex(string root, string input, string output)
        {
            string xexTool = Environment.GetEnvironmentVariable("XEXTOOL");
            if (String.IsNullOrEmpty(xexTool) || !File.Exists(xexTool))
            {
                string local = Path.Combine(root, "tools", "xextool.exe");
                xexTool = File.Exists(local) ? local : @"C:\Xbox\_PROGRAMS\xextool.exe";
            }
            RequireFile(xexTool);
            if (File.Exists(output))
                File.Delete(output);

            var start = new ProcessStartInfo
            {
                FileName = xexTool,
                Arguments = "-r a -e u -o \"" + output + "\" \"" + input + "\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using (Process process = Process.Start(start))
            {
                string standardOutput = process.StandardOutput.ReadToEnd();
                string standardError = process.StandardError.ReadToEnd();
                process.WaitForExit();
                if (process.ExitCode != 0 || !File.Exists(output))
                    throw new InvalidOperationException("xextool failed (" + process.ExitCode + "): " +
                        standardOutput + standardError);
            }
        }

        private static void VerifyPackage(string path,
                                          IDictionary<string, string> sources,
                                          string verifyDirectory)
        {
            if (Directory.Exists(verifyDirectory))
                Directory.Delete(verifyDirectory, true);
            Directory.CreateDirectory(verifyDirectory);

            var log = new LogRecord();
            var package = new STFSPackage(path, log);
            try
            {
                if (!package.ParseSuccess)
                    throw new InvalidDataException("The completed STFS package could not be reopened.");

                foreach (Verified check in package.VerifyHeader())
                    if (!check.IsValid &&
                        !String.Equals(check.ThisType.ToString(), "Signature",
                                       StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException("STFS header verification failed: " + check.ThisType);
                foreach (Verified check in package.VerifyHashTables())
                    if (!check.IsValid)
                        throw new InvalidDataException("STFS hash-table verification failed: " + check.ThisType);

                FileEntry[] entries = package.RootDirectory.GetSubFiles();
                foreach (KeyValuePair<string, string> source in sources)
                {
                    FileEntry match = null;
                    foreach (FileEntry entry in entries)
                    {
                        if (String.Equals(entry.Name, source.Key, StringComparison.OrdinalIgnoreCase))
                        {
                            match = entry;
                            break;
                        }
                    }
                    if (match == null)
                        throw new InvalidDataException("The completed package is missing " + source.Key + ".");

                    string extracted = Path.Combine(verifyDirectory, source.Key);
                    if (!match.Extract(extracted) || !FilesEqual(source.Value, extracted))
                        throw new InvalidDataException("Byte verification failed for " + source.Key + ".");
                }
            }
            finally
            {
                package.CloseIO();
            }
        }

        private static void RequireFile(string path)
        {
            if (!File.Exists(path))
                throw new FileNotFoundException("Missing required build input.", path);
        }

        private static bool FilesEqual(string left, string right)
        {
            var leftInfo = new FileInfo(left);
            var rightInfo = new FileInfo(right);
            if (leftInfo.Length != rightInfo.Length)
                return false;

            using (FileStream a = File.OpenRead(left))
            using (FileStream b = File.OpenRead(right))
            {
                var aBuffer = new byte[8192];
                var bBuffer = new byte[8192];
                int count;
                while ((count = a.Read(aBuffer, 0, aBuffer.Length)) != 0)
                {
                    if (b.Read(bBuffer, 0, count) != count)
                        return false;
                    for (int i = 0; i < count; ++i)
                        if (aBuffer[i] != bBuffer[i])
                            return false;
                }
            }
            return true;
        }
    }
}
