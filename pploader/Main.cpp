#include "stdafx.h"

extern "C" {
    NTSYSAPI VOID NTAPI DbgPrint(const char* Format, ...);

    NTSYSAPI DWORD NTAPI ExCreateThread(
        PHANDLE ThreadHandle,
        DWORD StackSize,
        LPDWORD ThreadId,
        PVOID ApiThreadStartup,
        LPTHREAD_START_ROUTINE StartAddress,
        LPVOID Parameter,
        DWORD CreationFlags);

    NTSYSAPI NTSTATUS NTAPI XexLoadImage(
        LPCSTR XexName,
        DWORD ModuleTypeFlags,
        DWORD MinimumVersion,
        PHANDLE ModuleHandle);

    NTSYSAPI VOID NTAPI XexUnloadImageAndExitThread(
        HANDLE ModuleHandle,
        DWORD ExitCode);

    NTSYSAPI NTSTATUS NTAPI KeWaitForSingleObject(
        PVOID Object,
        DWORD WaitReason,
        DWORD WaitMode,
        BOOL Alertable,
        PLARGE_INTEGER Timeout);

    extern PVOID* UsbdBootEnumerationDoneEvent;

    NTSYSAPI HRESULT NTAPI ObCreateSymbolicLink(
        PVOID SymbolicLinkName,
        PVOID DeviceName);

    NTSYSAPI VOID NTAPI RtlInitAnsiString(
        PVOID DestinationString,
        LPCSTR SourceString);

    VOID XapiThreadStartup(
        VOID (__cdecl *StartRoutine)(VOID*),
        PVOID StartContext,
        DWORD ExitCode);
}

namespace {

const DWORD kPluginCount = 5;
const DWORD kMaximumIniSize = 16 * 1024;
const DWORD kPluginModuleType = 8;
const DWORD kRuntimeSettleDelayMs = 1000;
const DWORD kUserRequestWaitReason = 3;
const DWORD kUserWaitMode = 1;
const NTSTATUS kStatusObjectNameCollision = (NTSTATUS)0xC0000035;
const CHAR kDefaultIniPath[] = "Hdd:\\" PPLOADER_INI_NAME;
const CHAR kDefaultIniContents[] =
    "[Plugins]\r\n"
    "Plugin1 = Hdd:\\PeerPressure\\xbdm.xex\r\n"
    "Plugin2 = \r\n"
    "Plugin3 = \r\n"
    "Plugin4 = \r\n"
    "Plugin5 = ";

HANDLE g_ModuleHandle = NULL;

struct KernelAnsiString {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
};

CHAR* TrimLeft(CHAR* Text) {
    while (*Text == ' ' || *Text == '\t')
        ++Text;

    return Text;
}

VOID TrimRight(CHAR* Text) {
    DWORD Length = (DWORD)strlen(Text);

    while (Length != 0) {
        CHAR Value = Text[Length - 1];

        if (Value != ' ' && Value != '\t' && Value != '\r' && Value != '\n')
            break;

        Text[--Length] = 0;
    }
}

VOID RemoveInlineComment(CHAR* Text) {
    for (CHAR* Cursor = Text; *Cursor != 0; ++Cursor) {
        if ((*Cursor == ';' || *Cursor == '#') &&
            (Cursor == Text || Cursor[-1] == ' ' || Cursor[-1] == '\t')) {
            *Cursor = 0;
            TrimRight(Text);
            return;
        }
    }
}

BOOL FileExists(LPCSTR Path) {
    DWORD Attributes = GetFileAttributesA(Path);
    return Attributes != INVALID_FILE_ATTRIBUTES &&
        (Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

BOOL CopyString(CHAR* Destination, DWORD DestinationSize, LPCSTR Source) {
    DWORD Length = Source != NULL ? (DWORD)strlen(Source) : 0;

    if (Destination == NULL || DestinationSize == 0 || Length >= DestinationSize)
        return FALSE;

    if (Length != 0)
        memcpy(Destination, Source, Length);

    Destination[Length] = 0;
    return TRUE;
}

BOOL CreateAliasInNamespace(
    LPCSTR Namespace,
    LPCSTR Alias,
    LPCSTR DevicePath) {
    CHAR LinkPath[MAX_PATH];

    if (strlen(Namespace) + strlen(Alias) >= ARRAYSIZE(LinkPath))
        return FALSE;

    strcpy_s(LinkPath, ARRAYSIZE(LinkPath), Namespace);
    strcat_s(LinkPath, ARRAYSIZE(LinkPath), Alias);

    KernelAnsiString LinkName;
    KernelAnsiString DeviceName;
    RtlInitAnsiString(&LinkName, LinkPath);
    RtlInitAnsiString(&DeviceName, DevicePath);

    NTSTATUS Status = (NTSTATUS)ObCreateSymbolicLink(&LinkName, &DeviceName);

    // A collision means another component already created this exact alias.
    return NT_SUCCESS(Status) || Status == kStatusObjectNameCollision;
}

VOID EnsureDriveAliases() {
    struct DriveAlias {
        LPCSTR Alias;
        LPCSTR DevicePath;
    };

    static const DriveAlias Aliases[] = {
        { "Hdd:",  "\\Device\\Harddisk0\\Partition1" },
        { "Usb:",  "\\Device\\Mass0" },
        { "Usb0:", "\\Device\\Mass0" }
    };

    for (DWORD Index = 0; Index < ARRAYSIZE(Aliases); ++Index) {
        BOOL SystemResult = CreateAliasInNamespace(
            "\\System??\\",
            Aliases[Index].Alias,
            Aliases[Index].DevicePath);
        BOOL UserResult = CreateAliasInNamespace(
            "\\??\\",
            Aliases[Index].Alias,
            Aliases[Index].DevicePath);

        (void)SystemResult;
        (void)UserResult;
    }
}

BOOL FindIniPath(CHAR* Path, DWORD PathSize) {
    static const CHAR* IniPaths[] = {
        "Usb:\\" PPLOADER_INI_NAME,
        "Hdd:\\" PPLOADER_INI_NAME
    };

    for (DWORD Index = 0; Index < ARRAYSIZE(IniPaths); ++Index) {
        if (FileExists(IniPaths[Index]) &&
            CopyString(Path, PathSize, IniPaths[Index]))
            return TRUE;
    }

    Path[0] = 0;
    return FALSE;
}

BOOL WriteDefaultIni() {
    HANDLE File = CreateFileA(
        kDefaultIniPath,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (File == INVALID_HANDLE_VALUE)
        return FileExists(kDefaultIniPath);

    const DWORD Size = sizeof(kDefaultIniContents) - 1;
    DWORD TotalWritten = 0;
    BOOL Result = TRUE;

    while (TotalWritten < Size) {
        DWORD Written = 0;

        if (!WriteFile(
                File,
                kDefaultIniContents + TotalWritten,
                Size - TotalWritten,
                &Written,
                NULL) || Written == 0) {
            Result = FALSE;
            break;
        }

        TotalWritten += Written;
    }

    CloseHandle(File);

    if (!Result) {
        DeleteFileA(kDefaultIniPath);
        return FALSE;
    }

    return TRUE;
}

BOOL ReadIniFile(LPCSTR Path, CHAR* Buffer, DWORD BufferSize) {
    HANDLE File = CreateFileA(
        Path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    DWORD HighSize = 0;
    DWORD Size = GetFileSize(File, &HighSize);

    if (Size == INVALID_FILE_SIZE || HighSize != 0 || Size >= BufferSize) {
        CloseHandle(File);
        return FALSE;
    }

    DWORD TotalRead = 0;
    BOOL Result = TRUE;

    while (TotalRead < Size) {
        DWORD Read = 0;

        if (!ReadFile(File, Buffer + TotalRead, Size - TotalRead, &Read, NULL) || Read == 0) {
            Result = FALSE;
            break;
        }

        TotalRead += Read;
    }

    CloseHandle(File);

    if (!Result)
        return FALSE;

    Buffer[TotalRead] = 0;
    return TRUE;
}

INT GetPluginIndex(LPCSTR Key) {
    if (_strnicmp(Key, "Plugin", 6) != 0)
        return -1;

    if (Key[6] < '1' || Key[6] > '5' || Key[7] != 0)
        return -1;

    return Key[6] - '1';
}

VOID ParsePluginIni(CHAR* Buffer, CHAR Plugins[kPluginCount][MAX_PATH]) {
    BOOL InPluginsSection = FALSE;
    CHAR* Cursor = Buffer;

    while (*Cursor != 0) {
        CHAR* Line = Cursor;

        while (*Cursor != 0 && *Cursor != '\r' && *Cursor != '\n')
            ++Cursor;

        if (*Cursor != 0) {
            *Cursor++ = 0;

            while (*Cursor == '\r' || *Cursor == '\n')
                ++Cursor;
        }

        Line = TrimLeft(Line);
        TrimRight(Line);

        if (*Line == 0 || *Line == ';' || *Line == '#')
            continue;

        if (*Line == '[') {
            CHAR* Close = strchr(Line + 1, ']');

            if (Close == NULL) {
                InPluginsSection = FALSE;
                continue;
            }

            *Close = 0;
            CHAR* Section = TrimLeft(Line + 1);
            TrimRight(Section);
            InPluginsSection = _stricmp(Section, "Plugins") == 0;
            continue;
        }

        if (!InPluginsSection)
            continue;

        CHAR* Equals = strchr(Line, '=');

        if (Equals == NULL)
            continue;

        *Equals = 0;
        CHAR* Key = TrimLeft(Line);
        CHAR* Value = TrimLeft(Equals + 1);
        TrimRight(Key);
        TrimRight(Value);
        RemoveInlineComment(Value);

        INT PluginIndex = GetPluginIndex(Key);

        if (PluginIndex < 0)
            continue;

        // An empty value intentionally disables this slot.
        if (*Value == 0) {
            Plugins[PluginIndex][0] = 0;
            continue;
        }

        if (!CopyString(Plugins[PluginIndex], MAX_PATH, Value)) {
            Plugins[PluginIndex][0] = 0;
        }
    }
}

DWORD UnloadSelfAndExitThread(DWORD ExitCode) {
    HANDLE ModuleHandle = g_ModuleHandle;

    if (ModuleHandle != NULL) {
        DbgPrint("[pploader] Unloading\n");
        XexUnloadImageAndExitThread(ModuleHandle, ExitCode);
    }

    // XexUnloadImageAndExitThread normally does not return. Keep a normal
    // thread return as a safe fallback if no module handle was captured.
    return ExitCode;
}

BOOL WaitForRuntimeReady() {
    DbgPrint("[pploader] Waiting to init.\n");
    NTSTATUS Status = KeWaitForSingleObject(
        UsbdBootEnumerationDoneEvent,
        kUserRequestWaitReason,
        kUserWaitMode,
        FALSE,
        NULL);

    (void)Status;

    // Match DashLaunch's firstRunTasks ordering: the USB boot-enumeration
    // barrier is followed by one second for storage and title startup.
    Sleep(kRuntimeSettleDelayMs);
    return TRUE;
}

DWORD WINAPI PluginLoaderThread(LPVOID) {
    // DashLaunch loads plugins after this same boot-enumeration barrier rather
    // than looking up dash.xex as a named system module.
    if (!WaitForRuntimeReady())
        return UnloadSelfAndExitThread(0);

    EnsureDriveAliases();

    CHAR IniPath[MAX_PATH];

    if (!FindIniPath(IniPath, ARRAYSIZE(IniPath))) {
        if (!WriteDefaultIni() ||
            !CopyString(IniPath, ARRAYSIZE(IniPath), kDefaultIniPath)) {
            return UnloadSelfAndExitThread(0);
        }
    }

    CHAR* IniBuffer = (CHAR*)XPhysicalAlloc(
        kMaximumIniSize + 1,
        MAXULONG_PTR,
        0,
        PAGE_READWRITE);

    if (IniBuffer == NULL) {
        return UnloadSelfAndExitThread(0);
    }

    if (!ReadIniFile(IniPath, IniBuffer, kMaximumIniSize + 1)) {
        XPhysicalFree(IniBuffer);
        return UnloadSelfAndExitThread(0);
    }

    CHAR Plugins[kPluginCount][MAX_PATH];
    memset(Plugins, 0, sizeof(Plugins));
    ParsePluginIni(IniBuffer, Plugins);
    XPhysicalFree(IniBuffer);

    for (DWORD Index = 0; Index < kPluginCount; ++Index) {
        LPCSTR PluginPath = Plugins[Index];

        if (*PluginPath == 0)
            continue;

        if (!FileExists(PluginPath)) {
            continue;
        }

        DbgPrint("[pploader] Loading plugin%d: %s\n", Index + 1, PluginPath);
        NTSTATUS Status = XexLoadImage(
            PluginPath,
            kPluginModuleType,
            0,
            NULL);

        if (!NT_SUCCESS(Status)) {
            DbgPrint(
                "[pploader] Failed to load plugin%d: 0x%08X\n",
                Index + 1,
                Status);
        }
    }

    return UnloadSelfAndExitThread(0);
}

BOOL StartPluginLoader() {
    HANDLE Thread = NULL;
    DWORD ThreadId = 0;
    NTSTATUS Status = ExCreateThread(
        &Thread,
        0,
        &ThreadId,
        (PVOID)XapiThreadStartup,
        PluginLoaderThread,
        NULL,
        0x2);

    if (!NT_SUCCESS(Status) || Thread == NULL) {
        return FALSE;
    }

    XSetThreadProcessor(Thread, 4);
    SetThreadPriority(Thread, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(Thread);
    return TRUE;
}

} // anonymous namespace

BOOL APIENTRY DllMain(HANDLE Module, DWORD Reason, LPVOID) {
    if (Reason == DLL_PROCESS_ATTACH) {
		g_ModuleHandle = Module;
        StartPluginLoader();
    }

    return TRUE;
}
