// FileExplorerMU.cpp
// XbDiag - Memory Unit mounting and formatting.
//
// MountAllDrives maps HDD partitions to letters C/E/F/G/X/Y/Z.
// MountMUs opens each present MU via MU_CreateDeviceObject and maps A-H.
// FormatMU dismounts, formats as FATX, and remaps the drive letter.

#include "FileExplorerMU.h"
#include "FileExplorer.h"
#include <xtl.h>
#include "input.h"

// ============================================================================
// Kernel exports
// ============================================================================

extern "C"
{
    LONG  WINAPI MU_CreateDeviceObject(DWORD port, DWORD slot, XBOX_STRING* deviceName);
    VOID  WINAPI MU_CloseDeviceObject(DWORD port, DWORD slot);
    LONG  WINAPI IoDismountVolume(void* deviceObject);
    VOID* WINAPI MU_GetExistingDeviceObject(DWORD port, DWORD slot);
    BOOL  WINAPI XapiFormatFATVolumeEx(XBOX_STRING* devicePath, DWORD clusterSize);
}

// ============================================================================
// Drive map
// ============================================================================

struct DriveMap { const char* letter; const char* device; };
static const DriveMap k_drives[] =
{
    { "C", "\\Device\\Harddisk0\\Partition2" },
    { "E", "\\Device\\Harddisk0\\Partition1" },
    { "F", "\\Device\\Harddisk0\\Partition6" },
    { "G", "\\Device\\Harddisk0\\Partition7" },
    { "X", "\\Device\\Harddisk0\\Partition3" },
    { "Y", "\\Device\\Harddisk0\\Partition4" },
    { "Z", "\\Device\\Harddisk0\\Partition5" },
};

static void MountHDDDrives()
{
    char linkBuf[16];
    for (int i = 0; i < 7; ++i)
    {
        linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
        linkBuf[4] = k_drives[i].letter[0];
        linkBuf[5] = ':'; linkBuf[6] = '\0';
        const char* dev = k_drives[i].device;
        int devLen = 0; while (dev[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), (char*)dev };
        IoCreateSymbolicLink(&sLink, &sDev);
    }
}

static bool MountMUs()
{
    bool anyMounted = false;
    for (int port = 0; port < 4; ++port)
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            if (!IsMUPresent(port, slot)) continue;

            char driveLetter = 'A' + (char)(port * 2 + slot);
            char devBuf[64];
            XBOX_STRING devName;
            devName.Length = 0;
            devName.MaximumLength = sizeof(devBuf) - 2;
            devName.Buffer = devBuf;

            if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0)
                continue;

            char linkBuf[8];
            linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
            linkBuf[4] = driveLetter; linkBuf[5] = ':'; linkBuf[6] = '\0';
            XBOX_STRING sLink = { 6, 7, linkBuf };
            IoCreateSymbolicLink(&sLink, &devName);
            anyMounted = true;
        }
    }
    return anyMounted;
}

// ============================================================================
// Public API
// ============================================================================

bool FE_MU_MountAll()
{
    MountHDDDrives();
    return MountMUs();
}

bool FE_MU_Format(int port, int slot)
{
    void* devObj = MU_GetExistingDeviceObject((DWORD)port, (DWORD)slot);
    if (devObj) IoDismountVolume(devObj);

    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);

    char devBuf[64];
    XBOX_STRING devName;
    devName.Length = 0;
    devName.MaximumLength = sizeof(devBuf) - 2;
    devName.Buffer = devBuf;

    if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0)
        return false;

    BOOL ok = XapiFormatFATVolumeEx(&devName, 0);

    char driveLetter = 'A' + (char)(port * 2 + slot);
    char linkBuf[8];
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = driveLetter; linkBuf[5] = ':'; linkBuf[6] = '\0';
    XBOX_STRING sLink = { 6, 7, linkBuf };
    IoCreateSymbolicLink(&sLink, &devName);

    return ok != FALSE;
}