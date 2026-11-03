#ifndef EFI_H
#define EFI_H
/* Minimal UEFI definitions: just what loader.c needs, with exact layouts
 * (unused boot-service slots are void* placeholders to keep offsets). */
#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint64_t UINTN;
typedef uint16_t CHAR16;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS             0ULL
#define EFI_ERR(x)              (0x8000000000000000ULL | (x))
#define EFI_BUFFER_TOO_SMALL    EFI_ERR(5)
#define EFI_ERROR(s)            ((s) & 0x8000000000000000ULL)

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

typedef struct { uint64_t Signature; uint32_t Revision, HeaderSize, CRC32, Reserved; } EFI_TABLE_HEADER;

typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
typedef enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
    EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory
} EFI_MEMORY_TYPE;

typedef struct {
    uint32_t Type; uint32_t Pad;
    uint64_t PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;

struct EFI_SIMPLE_TEXT_OUTPUT;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(struct EFI_SIMPLE_TEXT_OUTPUT *, const CHAR16 *);
typedef struct EFI_SIMPLE_TEXT_OUTPUT {
    void *Reset;
    EFI_TEXT_STRING OutputString;
} EFI_SIMPLE_TEXT_OUTPUT;

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, uint32_t *);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    void *GetNextMonotonicCount, *Stall;
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, uint64_t, UINTN, const CHAR16 *);
    void *ConnectController, *DisconnectController, *OpenProtocol, *CloseProtocol,
         *OpenProtocolInformation, *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *, void *, void **);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor; uint32_t FirmwareRevision; uint32_t Pad;
    EFI_HANDLE ConsoleInHandle; void *ConIn;
    EFI_HANDLE ConsoleOutHandle; EFI_SIMPLE_TEXT_OUTPUT *ConOut;
    EFI_HANDLE StandardErrorHandle; EFI_SIMPLE_TEXT_OUTPUT *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries; void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    uint32_t Revision; uint32_t Pad;
    EFI_HANDLE ParentHandle; EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE;

struct EFI_FILE;
typedef struct EFI_FILE {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE *, struct EFI_FILE **, const CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE *);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE *, UINTN *, void *);
    void *Write;
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE *, uint64_t *);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE *, uint64_t);
} EFI_FILE;

typedef struct EFI_SIMPLE_FILE_SYSTEM {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM *, EFI_FILE **);
} EFI_SIMPLE_FILE_SYSTEM;

#define EFI_FILE_MODE_READ 1ULL

typedef enum { PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor,
               PixelBitMask, PixelBltOnly } EFI_GRAPHICS_PIXEL_FORMAT;
typedef struct { uint32_t RedMask, GreenMask, BlueMask, ReservedMask; } EFI_PIXEL_BITMASK;
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {
    uint32_t MaxMode, Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_MODE;
typedef struct EFI_GRAPHICS_OUTPUT {
    void *QueryMode;
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT *, uint32_t);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_MODE *Mode;
} EFI_GRAPHICS_OUTPUT;

#endif
