#include <xtl.h>
#include <vector>
#include <string>
#include "AtgConsole.h"

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

typedef LONG NTSTATUS;
typedef struct _ANSI_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} ANSI_STRING, *PANSI_STRING;

typedef struct _XBOX_KRNL_VERSION
{
    USHORT Major;
    USHORT Minor;
    USHORT Build;
    USHORT Qfe;
} XBOX_KRNL_VERSION, *PXBOX_KRNL_VERSION;

#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)

extern "C"
{
    VOID RtlInitAnsiString(PANSI_STRING DestinationString, const char* SourceString);
    NTSTATUS ObCreateSymbolicLink(PANSI_STRING SymbolicLinkName,
                                  PANSI_STRING DeviceName);
    NTSTATUS XexLoadImage(LPCSTR XexName, DWORD ModuleTypeFlags,
                          DWORD MinimumVersion, PHANDLE ModuleHandle);
    DWORD ExCreateThread(PHANDLE ThreadHandle, DWORD StackSize,
                         LPDWORD ThreadId, PVOID ApiThreadStartup,
                         LPTHREAD_START_ROUTINE StartAddress,
                         LPVOID Parameter, DWORD CreationFlags);
    VOID XapiThreadStartup(VOID (__cdecl *StartRoutine)(VOID*),
                           PVOID StartContext, DWORD ExitCode);
    VOID HalReturnToFirmware(DWORD PowerDownMode);
    extern PXBOX_KRNL_VERSION XboxKrnlVersion;
}

namespace
{
    const char* const kPatchPath = "Hdd:\\PeerPressure\\System\\kernel_patches.bin";
    const char* const kPatchTempPath = "Hdd:\\PeerPressure\\System\\kernel_patches.ppnew";
    const char* const kSourceXbdm = "game:\\xbdm.xex";
    const char* const kSourceLoader = "game:\\pploader.xex";
    const char* const kTargetXbdm = "Hdd:\\PeerPressure\\xbdm.xex";
    const char* const kTargetLoader = "Hdd:\\PeerPressure\\pploader.xex";

    const DWORD kLoaderAddress = 0x801616C4;
    const DWORD kHookAddress = 0x80061414;
    const DWORD kHalRebootRoutine = 1;
    const DWORD kModuleLoadTimeoutMs = 5000;
    const NTSTATUS kStatusIoTimeout = (NTSTATUS)0xC00000B5;

    const BYTE kLoaderBytes[] = {
        0x3D,0x60,0x80,0x16,0x61,0x63,0x16,0xE4,
        0x38,0x80,0x00,0x08,0x38,0xA0,0x00,0x00,
        0x38,0xC0,0x00,0x00,0x4B,0xF1,0xC0,0xE9,
        0x38,0x60,0x00,0x00,0x4B,0xEF,0xFD,0x38,
        0x5C,0x44,0x65,0x76,0x69,0x63,0x65,0x5C,
        0x48,0x61,0x72,0x64,0x64,0x69,0x73,0x6B,
        0x30,0x5C,0x50,0x61,0x72,0x74,0x69,0x74,
        0x69,0x6F,0x6E,0x31,0x5C,0x50,0x65,0x65,
        0x72,0x50,0x72,0x65,0x73,0x73,0x75,0x72,
        0x65,0x5C,0x70,0x70,0x6C,0x6F,0x61,0x64,
        0x65,0x72,0x2E,0x78,0x65,0x78,0x00,0x00
    };
    const BYTE kHookBytes[] = { 0x48,0x10,0x02,0xB1 };

    enum InstallState
    {
        StateUnavailable,
        StateNotInstalled,
        StateInstalled,
        StateUpdate,
        StateRepair,
        StateRepairUpdate,
        StateJustInstalled,
        StateJustUpdated,
        StateLoadFailed,
        StateJustUninstalled
    };

    struct PatchRecord
    {
        DWORD address;
        DWORD length;
        std::vector<BYTE> data;
    };

    ATG::Console g_console;
    InstallState g_state = StateUnavailable;
    std::string g_status;
    WORD g_previousButtons = 0;
    DWORD g_packagedVersion = 0;
    DWORD g_installedVersion = 0;
    bool g_packagedVersionValid = false;
    bool g_installedVersionValid = false;
    volatile BOOL g_loadFinished = FALSE;
    volatile NTSTATUS g_loadStatus = 0;

    DWORD ReadBE32(const BYTE* p)
    {
        return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) |
               ((DWORD)p[2] << 8) | (DWORD)p[3];
    }

    void WriteBE32(std::vector<BYTE>& output, DWORD value)
    {
        output.push_back((BYTE)(value >> 24));
        output.push_back((BYTE)(value >> 16));
        output.push_back((BYTE)(value >> 8));
        output.push_back((BYTE)value);
    }

    DWORD Align4(DWORD value)
    {
        return (value + 3) & ~3;
    }

    bool FileExists(const char* path)
    {
        DWORD attributes = GetFileAttributes(path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool ReadWholeFile(const char* path, std::vector<BYTE>& output)
    {
        HANDLE file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD size = GetFileSize(file, NULL);
        if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024)
        {
            CloseHandle(file);
            SetLastError(ERROR_BAD_LENGTH);
            return false;
        }

        output.resize(size);
        DWORD done = 0;
        BOOL ok = size == 0 || ReadFile(file, &output[0], size, &done, NULL);
        CloseHandle(file);
        if (!ok || done != size)
        {
            SetLastError(ERROR_READ_FAULT);
            return false;
        }
        return true;
    }

    bool ReadXexVersion(const char* path, DWORD& version)
    {
        std::vector<BYTE> image;
        version = 0;
        if (!ReadWholeFile(path, image) || image.size() < 0x18 ||
            memcmp(&image[0], "XEX2", 4) != 0)
            return false;

        DWORD headerCount = ReadBE32(&image[0x14]);
        if (headerCount > (image.size() - 0x18) / 8)
            return false;

        for (DWORD i = 0; i < headerCount; ++i)
        {
            size_t entry = 0x18 + (size_t)i * 8;
            if (ReadBE32(&image[entry]) != 0x00040006)
                continue;

            DWORD executionId = ReadBE32(&image[entry + 4]);
            if (executionId > image.size() || image.size() - executionId < 8)
                return false;
            version = ReadBE32(&image[executionId + 4]);
            return true;
        }
        return false;
    }

    void FormatXexVersion(DWORD version, char* output, size_t outputSize)
    {
        sprintf_s(output, outputSize, "%u.%u.%u.%u",
                  (version >> 28) & 0xF,
                  (version >> 24) & 0xF,
                  (version >> 8) & 0xFFFF,
                  version & 0xFF);
    }

    bool WriteWholeFileAtomic(const char* path, const char* tempPath,
                              const std::vector<BYTE>& data)
    {
        DeleteFile(tempPath);
        HANDLE file = CreateFile(tempPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD done = 0;
        BOOL ok = data.empty() ||
                  WriteFile(file, &data[0], (DWORD)data.size(), &done, NULL);
        if (ok)
            ok = FlushFileBuffers(file);
        CloseHandle(file);

        if (!ok || done != data.size())
        {
            DeleteFile(tempPath);
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }

        if (!MoveFileEx(tempPath, path,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFile(tempPath);
            return false;
        }
        return true;
    }

    bool CopyFileAtomic(const char* source, const char* target)
    {
        std::string temp(target);
        temp += ".ppnew";
        DeleteFile(temp.c_str());
        if (!CopyFile(source, temp.c_str(), FALSE))
            return false;
        if (!MoveFileEx(temp.c_str(), target,
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFile(temp.c_str());
            return false;
        }
        return true;
    }

    bool MountHdd()
    {
        ANSI_STRING linkName;
        ANSI_STRING deviceName;
        RtlInitAnsiString(&linkName, "\\??\\Hdd:");
        RtlInitAnsiString(&deviceName, "\\Device\\Harddisk0\\Partition1");
        NTSTATUS status = ObCreateSymbolicLink(&linkName, &deviceName);
        return status >= 0 || status == STATUS_OBJECT_NAME_COLLISION;
    }

    bool ParsePatchFile(const std::vector<BYTE>& input,
                        std::vector<PatchRecord>& records,
                        std::string& error)
    {
        records.clear();
        if (input.size() < 4)
        {
            error = "kernel_patches.bin is too small.";
            return false;
        }

        size_t offset = 0;
        while (offset + 4 <= input.size())
        {
            DWORD address = ReadBE32(&input[offset]);
            if (address == 0xFFFFFFFF)
                return true;
            if (offset + 4 == input.size() &&
                (address == kLoaderAddress || address == kHookAddress))
            {
                PatchRecord partial;
                partial.address = address;
                partial.length = 0;
                records.push_back(partial);
                return true;
            }
            if (offset + 8 > input.size())
                break;

            DWORD length = ReadBE32(&input[offset + 4]);
            DWORD storedLength = Align4(length);
            if (!length || storedLength < length ||
                offset + 8 + storedLength > input.size())
                break;

            PatchRecord record;
            record.address = address;
            record.length = length;
            record.data.assign(input.begin() + offset + 8,
                               input.begin() + offset + 8 + storedLength);
            records.push_back(record);
            offset += 8 + storedLength;
        }

        error = "kernel_patches.bin has a malformed record or no terminator.";
        records.clear();
        return false;
    }

    bool RecordMatches(const PatchRecord& record, DWORD address,
                       const BYTE* bytes, DWORD length)
    {
        return record.address == address && record.length == length &&
               record.data.size() >= length &&
               memcmp(&record.data[0], bytes, length) == 0;
    }

    void CountTargets(const std::vector<PatchRecord>& records,
                      int& loaderExact, int& hookExact,
                      int& loaderOther, int& hookOther)
    {
        loaderExact = hookExact = loaderOther = hookOther = 0;
        for (size_t i = 0; i < records.size(); ++i)
        {
            if (records[i].address == kLoaderAddress)
            {
                if (RecordMatches(records[i], kLoaderAddress, kLoaderBytes,
                                  sizeof(kLoaderBytes)))
                    ++loaderExact;
                else
                    ++loaderOther;
            }
            else if (records[i].address == kHookAddress)
            {
                if (RecordMatches(records[i], kHookAddress, kHookBytes,
                                  sizeof(kHookBytes)))
                    ++hookExact;
                else
                    ++hookOther;
            }
        }
    }

    void AppendRecord(std::vector<BYTE>& output, DWORD address,
                      const BYTE* bytes, DWORD length)
    {
        WriteBE32(output, address);
        WriteBE32(output, length);
        output.insert(output.end(), bytes, bytes + length);
        while (output.size() & 3)
            output.push_back(0);
    }

    bool BuildPatchedFile(const std::vector<PatchRecord>& records,
                          bool install, std::vector<BYTE>& output)
    {
        output.clear();
        size_t expectedSize = 4;
        for (size_t i = 0; i < records.size(); ++i)
        {
            if (records[i].address != kLoaderAddress &&
                records[i].address != kHookAddress)
                expectedSize += 8 + records[i].data.size();
        }
        if (install)
        {
            expectedSize += 8 + Align4(sizeof(kLoaderBytes));
            expectedSize += 8 + Align4(sizeof(kHookBytes));
        }
        output.reserve(expectedSize);

        for (size_t i = 0; i < records.size(); ++i)
        {
            if (records[i].address == kLoaderAddress ||
                records[i].address == kHookAddress)
                continue;
            WriteBE32(output, records[i].address);
            WriteBE32(output, records[i].length);
            output.insert(output.end(), records[i].data.begin(),
                          records[i].data.end());
        }

        if (install)
        {
            AppendRecord(output, kLoaderAddress, kLoaderBytes,
                         sizeof(kLoaderBytes));
            AppendRecord(output, kHookAddress, kHookBytes,
                         sizeof(kHookBytes));
        }
        WriteBE32(output, 0xFFFFFFFF);
        if (output.size() != expectedSize || output.size() < 4 ||
            ReadBE32(&output[output.size() - 4]) != 0xFFFFFFFF)
        {
            output.clear();
            return false;
        }
        return true;
    }

    bool LoadRecords(std::vector<PatchRecord>& records, std::string& error)
    {
        std::vector<BYTE> input;
        if (!ReadWholeFile(kPatchPath, input))
        {
            char message[160];
            sprintf_s(message, "Could not read kernel_patches.bin (0x%08X).",
                      GetLastError());
            error = message;
            return false;
        }
        return ParsePatchFile(input, records, error);
    }

    void RefreshState()
    {
        g_packagedVersionValid = ReadXexVersion(kSourceLoader, g_packagedVersion);
        g_installedVersionValid = ReadXexVersion(kTargetLoader, g_installedVersion);

        if (!FileExists(kPatchPath))
        {
            g_state = StateUnavailable;
            g_status = "PeerPressure not detected";
            return;
        }

        std::vector<PatchRecord> records;
        std::string error;
        if (!LoadRecords(records, error))
        {
            g_state = StateUnavailable;
            g_status = error;
            return;
        }

        int loaderExact, hookExact, loaderOther, hookOther;
        CountTargets(records, loaderExact, hookExact, loaderOther, hookOther);
        bool payloadsPresent = FileExists(kTargetXbdm) && FileExists(kTargetLoader);

        if (loaderExact == 1 && hookExact == 1 &&
            loaderOther == 0 && hookOther == 0 && payloadsPresent)
        {
            if (g_packagedVersionValid &&
                (!g_installedVersionValid ||
                 g_packagedVersion > g_installedVersion))
            {
                g_state = StateUpdate;
                g_status = "A newer PPLoader version is available.";
            }
            else
            {
                g_state = StateInstalled;
                g_status = "Loader patch and payload files are installed.";
            }
        }
        else if (!loaderExact && !hookExact && !loaderOther && !hookOther)
        {
            g_state = StateNotInstalled;
            g_status = "Loader patch is not installed.";
        }
        else
        {
            bool updateAvailable = FileExists(kTargetLoader) &&
                g_packagedVersionValid && g_installedVersionValid &&
                g_packagedVersion > g_installedVersion;
            g_state = updateAvailable ? StateRepairUpdate : StateRepair;
            g_status = updateAvailable
                ? "Bad installation and an updated PPLoader were detected."
                : "Partial, conflicting, or incomplete install detected.";
        }
    }

    bool EnsureDirectories()
    {
        if (!CreateDirectory("Hdd:\\PeerPressure", NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
        if (!CreateDirectory("Hdd:\\PeerPressure\\System", NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
        return true;
    }

    bool InstallOrRepair(std::string& result)
    {
        bool repairing = g_state == StateRepair || g_state == StateRepairUpdate;
        bool updating = g_state == StateUpdate || g_state == StateRepairUpdate;
        std::vector<PatchRecord> records;
        if (!LoadRecords(records, result))
            return false;
        if (!FileExists(kSourceXbdm) || !FileExists(kSourceLoader))
        {
            result = "Installer payload is incomplete: xbdm.xex or pploader.xex is missing.";
            return false;
        }
        if (!EnsureDirectories())
        {
            result = "Could not create the PeerPressure directories.";
            return false;
        }

        if (!CopyFileAtomic(kSourceXbdm, kTargetXbdm))
        {
            char message[128];
            sprintf_s(message, "Could not copy xbdm.xex (0x%08X).", GetLastError());
            result = message;
            return false;
        }
        if (!CopyFileAtomic(kSourceLoader, kTargetLoader))
        {
            char message[128];
            sprintf_s(message, "Could not copy pploader.xex (0x%08X).", GetLastError());
            result = message;
            return false;
        }

        std::vector<BYTE> output;
        if (!BuildPatchedFile(records, true, output))
        {
            result = "Could not serialize the complete loader patch file.";
            return false;
        }
        if (!WriteWholeFileAtomic(kPatchPath, kPatchTempPath, output))
        {
            char message[160];
            sprintf_s(message, "Could not update kernel_patches.bin (0x%08X).",
                      GetLastError());
            result = message;
            return false;
        }
        if (repairing && updating)
            result = "Repair and update completed.";
        else if (repairing)
            result = "Repair completed.";
        else if (updating)
            result = "Update completed.";
        else
            result = "Install completed.";
        return true;
    }

    bool Uninstall(std::string& result)
    {
        std::vector<PatchRecord> records;
        if (!LoadRecords(records, result))
            return false;

        std::vector<BYTE> output;
        if (!BuildPatchedFile(records, false, output))
        {
            result = "Could not serialize the cleaned patch file.";
            return false;
        }
        if (!WriteWholeFileAtomic(kPatchPath, kPatchTempPath, output))
        {
            char message[160];
            sprintf_s(message, "Could not update kernel_patches.bin (0x%08X).",
                      GetLastError());
            result = message;
            return false;
        }

        if (FileExists(kTargetLoader) && !DeleteFile(kTargetLoader))
        {
            char message[160];
            sprintf_s(message, "Patches removed, but pploader.xex delete failed (0x%08X).",
                      GetLastError());
            result = message;
            return false;
        }
        result = "Uninstall completed.";
        return true;
    }

    DWORD WINAPI LoadPPLoaderThread(LPVOID)
    {
        g_loadStatus = XexLoadImage(kTargetLoader, 8, 0, NULL);
        g_loadFinished = TRUE;
        return 0;
    }

    bool LoadPPLoaderNow(std::string& result)
    {
        g_loadFinished = FALSE;
        g_loadStatus = 0;

        HANDLE thread = NULL;
        DWORD threadId = 0;
        NTSTATUS status = (NTSTATUS)ExCreateThread(
            &thread,
            0,
            &threadId,
            (PVOID)XapiThreadStartup,
            LoadPPLoaderThread,
            NULL,
            0x2);

        if (status >= 0 && thread != NULL)
        {
            XSetThreadProcessor(thread, 4);
            SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
            ResumeThread(thread);
            CloseHandle(thread);

            DWORD started = GetTickCount();
            while (!g_loadFinished &&
                   GetTickCount() - started < kModuleLoadTimeoutMs)
                Sleep(16);

            status = g_loadFinished ? g_loadStatus : kStatusIoTimeout;
        }

        if (status < 0)
        {
            char message[160];
            sprintf_s(message, "Could not load PPLoader (0x%08X).", status);
            result = message;
            return false;
        }
        return true;
    }

    void DrawScreen()
    {
        g_console.Clear();
        g_console.Format("\n  PeerPressure Loader Installer\n");
        g_console.Format("  =============================\n\n");
        g_console.Format("  %s\n\n", g_status.c_str());

        char version[32];
        if (g_packagedVersionValid)
        {
            FormatXexVersion(g_packagedVersion, version, sizeof(version));
            g_console.Format("  PPLoader Version: %s\n", version);
        }
        else
        {
            g_console.Format("  PPLoader Version: Unknown\n");
        }
        if (g_installedVersionValid &&
            (g_state == StateInstalled || g_state == StateUpdate ||
             g_state == StateRepair || g_state == StateRepairUpdate))
        {
            FormatXexVersion(g_installedVersion, version, sizeof(version));
            g_console.Format("  Installed Version: %s\n", version);
        }
        g_console.Format("\n");

        if (g_state == StateNotInstalled)
            g_console.Format("  [A] Install\n");

        if (g_state == StateUpdate)
            g_console.Format("  [A] Update\n");

        if (g_state == StateRepair)
            g_console.Format("  [A] Repair\n");

        if (g_state == StateRepairUpdate)
            g_console.Format("  [A] Repair and Update\n");

        if (g_state == StateInstalled)
            g_console.Format("  [Y] Uninstall\n");

        if (g_state == StateJustUpdated || g_state == StateJustUninstalled)
            g_console.Format("  [X] Reboot\n");

        if (g_state == StateJustInstalled)
            g_console.Format("  [B] Load PPLoader and Exit\n");
        else if (g_state == StateLoadFailed)
            g_console.Format("  [B] Reboot\n");
        else
            g_console.Format("  [B] Exit\n");
    }

    WORD ReadPressedButtons()
    {
        WORD buttons = 0;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
        {
            XINPUT_STATE state;
            ZeroMemory(&state, sizeof(state));
            if (XInputGetState(i, &state) == ERROR_SUCCESS)
                buttons |= state.Gamepad.wButtons;
        }
        WORD pressed = (buttons ^ g_previousButtons) & buttons;
        g_previousButtons = buttons;
        return pressed;
    }
}

void __cdecl main()
{
    if (FAILED(g_console.Create("game:\\Arial_12.xpr", 0xFF101510, 0xFFF2F2F2)))
        return;

    if (XboxKrnlVersion->Build != 17559)
    {
        char message[128];
        sprintf_s(message, "Unsupported kernel build %u; this patch requires retail 17559.",
                  XboxKrnlVersion->Build);
        g_state = StateUnavailable;
        g_status = message;
    }
    else if (!MountHdd())
    {
        g_state = StateUnavailable;
        g_status = "Could not mount Hdd:.";
    }
    else
    {
        RefreshState();
    }
    DrawScreen();

    for (;;)
    {
        WORD pressed = ReadPressedButtons();
        if (pressed & XINPUT_GAMEPAD_B)
        {
            if (g_state == StateJustInstalled)
            {
                std::string loadResult;
                if (!LoadPPLoaderNow(loadResult))
                {
                    g_state = StateLoadFailed;
                    g_status = "Failed to start PPLoader. A reboot is required.";
                    DrawScreen();
                    Sleep(16);
                    continue;
                }
            }
            else if (g_state == StateLoadFailed)
            {
                HalReturnToFirmware(kHalRebootRoutine);
                return;
            }
            XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
            return;
        }

        if ((pressed & XINPUT_GAMEPAD_X) &&
            (g_state == StateJustUpdated || g_state == StateJustUninstalled))
        {
            HalReturnToFirmware(kHalRebootRoutine);
            return;
        }

        std::string result;
        bool attempted = false;
        if ((pressed & XINPUT_GAMEPAD_A) &&
            (g_state == StateNotInstalled || g_state == StateUpdate ||
             g_state == StateRepair || g_state == StateRepairUpdate))
        {
            attempted = true;
            bool updating = g_state == StateUpdate || g_state == StateRepairUpdate;
            if (InstallOrRepair(result))
            {
                g_state = updating ? StateJustUpdated : StateJustInstalled;
                g_installedVersion = g_packagedVersion;
                g_installedVersionValid = g_packagedVersionValid;
            }
        }
        else if ((pressed & XINPUT_GAMEPAD_Y) &&
                 g_state == StateInstalled)
        {
            attempted = true;
            if (Uninstall(result))
            {
                g_state = StateJustUninstalled;
                g_installedVersion = 0;
                g_installedVersionValid = false;
            }
        }

        if (attempted)
        {
            g_status = result;
            DrawScreen();
        }
        Sleep(16);
    }
}
