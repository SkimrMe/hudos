#ifndef HUDOS_EFI_H
#define HUDOS_EFI_H

/* Minimal, self-contained UEFI headers for building a freestanding
 * UEFI application (aarch64 / ARM64 EFI) with clang + lld-link.
 * Only the structures/protocols HUDOS uses are typed; unused
 * BootServices/RuntimeServices members are kept as VOID* placeholders
 * so the table layout/offsets stay correct. */

#include <stdint.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int8_t   INT8;
typedef int16_t  INT16;
typedef int32_t  INT32;
typedef int64_t  INT64;
typedef uint64_t UINTN;
typedef int64_t  INTN;
typedef uint16_t CHAR16;
typedef uint8_t  BOOLEAN;
typedef void     VOID;
typedef void    *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uint64_t EFI_LBA;
typedef uint64_t EFI_TPL;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

#define IN
#define OUT
#define OPTIONAL
#define EFIAPI __attribute__((ms_abi))
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define EFI_ERROR(a) (((INTN)(a)) < 0)
#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR (0x8000000000000001ULL)
#define EFI_INVALID_PARAMETER (0x8000000000000002ULL)
#define EFI_UNSUPPORTED (0x8000000000000003ULL)
#define EFI_BAD_BUFFER_SIZE (0x8000000000000004ULL)
#define EFI_BUFFER_TOO_SMALL (0x8000000000000005ULL)
#define EFI_NOT_READY (0x8000000000000006ULL)
#define EFI_DEVICE_ERROR (0x8000000000000007ULL)
#define EFI_WRITE_PROTECTED (0x8000000000000008ULL)
#define EFI_OUT_OF_RESOURCES (0x8000000000000009ULL)
#define EFI_VOLUME_CORRUPTED (0x800000000000000aULL)
#define EFI_NO_MEDIA (0x800000000000000bULL)
#define EFI_MEDIA_CHANGED (0x800000000000000cULL)
#define EFI_NOT_FOUND (0x800000000000000dULL)
#define EFI_ACCESS_DENIED (0x800000000000000fULL)
#define EFI_NO_RESPONSE (0x8000000000000010ULL)
#define EFI_VOLUME_FULL (0x8000000000000019ULL)
#define EFI_ABORTED (0x8000000000000021ULL)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- forward/opaque protocol pointers we need ---- */
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL        EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_POINTER_PROTOCOL         EFI_SIMPLE_POINTER_PROTOCOL;
typedef struct _EFI_LOADED_IMAGE_PROTOCOL           EFI_LOADED_IMAGE_PROTOCOL;
typedef struct _EFI_BLOCK_IO_PROTOCOL               EFI_BLOCK_IO_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL     EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL      EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct _EFI_DEVICE_PATH_PROTOCOL            EFI_DEVICE_PATH_PROTOCOL;

/* ---- Graphics Output Protocol ---- */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3,
    PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32  Version;
    UINT32  HorizontalResolution;
    UINT32  VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32  PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN  SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    UINT8 Blue;
    UINT8 Green;
    UINT8 Red;
    UINT8 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN UINT32 ModeNumber);
typedef enum {
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiBltVideoToVideo,
    EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY,
    IN UINTN DestinationX, IN UINTN DestinationY,
    IN UINTN Width, IN UINTN Height,
    IN UINTN Delta);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT        Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      *Mode;
};

/* ---- Simple Pointer Protocol ---- */
typedef struct {
    UINT64    ResolutionX;
    UINT64    ResolutionY;
    UINT64    ResolutionZ;
    BOOLEAN   LeftButton;
    BOOLEAN   RightButton;
} EFI_SIMPLE_POINTER_MODE;

typedef struct {
    INT32   RelativeMovementX;
    INT32   RelativeMovementY;
    INT32   RelativeMovementZ;
    BOOLEAN LeftButton;
    BOOLEAN RightButton;
} EFI_SIMPLE_POINTER_STATE;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_POINTER_RESET)(
    IN EFI_SIMPLE_POINTER_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_POINTER_GET_STATE)(
    IN EFI_SIMPLE_POINTER_PROTOCOL *This,
    IN OUT EFI_SIMPLE_POINTER_STATE *State);

struct _EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_SIMPLE_POINTER_RESET       Reset;
    EFI_SIMPLE_POINTER_GET_STATE   GetState;
    EFI_SIMPLE_POINTER_MODE       *Mode;
};

/* ---- Absolute Pointer Protocol ----
   标准 QEMU `-device usb-tablet` 在 OVMF 下由 UsbMouseAbsolutePointerDxe
   以 EFI_ABSOLUTE_POINTER_PROTOCOL（绝对坐标）暴露；UsbMouseDxe 则以
   EFI_SIMPLE_POINTER_PROTOCOL（相对）暴露，两者互斥。绝对设备每次移动
   直接给出屏内坐标，最适合平板/虚拟机光标。定义严格按 EDK2。 */
typedef struct {
    UINT64    AbsoluteMinX;
    UINT64    AbsoluteMinY;
    UINT64    AbsoluteMinZ;
    UINT64    AbsoluteMaxX;
    UINT64    AbsoluteMaxY;
    UINT64    AbsoluteMaxZ;
    UINT32    Attributes;
} EFI_ABSOLUTE_POINTER_MODE;

typedef struct {
    UINT64    CurrentX;
    UINT64    CurrentY;
    UINT64    CurrentZ;
    UINT32    ActiveButtons;   /* bit0 = EFI_ABSP_TouchActive（主按钮按下） */
} EFI_ABSOLUTE_POINTER_STATE;

#define EFI_ABSP_TouchActive  0x00000001

/* 与 Simple Pointer 对齐：先前向声明 struct tag 并 typedef 协议类型 */
typedef struct _EFI_ABSOLUTE_POINTER_PROTOCOL EFI_ABSOLUTE_POINTER_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_ABSOLUTE_POINTER_RESET)(
    IN EFI_ABSOLUTE_POINTER_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_ABSOLUTE_POINTER_GET_STATE)(
    IN EFI_ABSOLUTE_POINTER_PROTOCOL *This,
    OUT EFI_ABSOLUTE_POINTER_STATE   *State);

typedef VOID *EFI_EVENT;

struct _EFI_ABSOLUTE_POINTER_PROTOCOL {
    EFI_ABSOLUTE_POINTER_RESET       Reset;
    EFI_ABSOLUTE_POINTER_GET_STATE   GetState;
    EFI_EVENT                        WaitForInput;
    EFI_ABSOLUTE_POINTER_MODE       *Mode;
};

/* ---- Loaded Image Protocol ---- */
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_ENTRY_POINT)(
    IN EFI_HANDLE ImageHandle,
    IN struct EFI_SYSTEM_TABLE_ *SystemTable);

typedef struct _EFI_LOADED_IMAGE_PROTOCOL {
    UINT32              Revision;
    EFI_HANDLE          ParentHandle;
    struct EFI_SYSTEM_TABLE_ *SystemTable;
    EFI_HANDLE          DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL *FilePath;
    VOID               *Reserved;
    UINT32              LoadOptionsSize;
    VOID               *LoadOptions;
    VOID               *ImageBase;
    UINT64              ImageSize;
    EFI_IMAGE_ENTRY_POINT EntryPoint;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- Block IO Protocol ---- */
typedef struct {
    UINT32    MediaId;
    BOOLEAN   RemovableMedia;
    BOOLEAN   MediaPresent;
    BOOLEAN   LogicalPartition;
    BOOLEAN   ReadOnly;
    BOOLEAN   WriteCaching;
    UINT32    BlockSize;
    UINT32    IoAlign;
    EFI_LBA   LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_RESET)(
    IN EFI_BLOCK_IO_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_READ)(
    IN EFI_BLOCK_IO_PROTOCOL *This,
    IN UINT32 MediaId,
    IN EFI_LBA Lba,
    IN UINTN BufferSize,
    OUT VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_WRITE)(
    IN EFI_BLOCK_IO_PROTOCOL *This,
    IN UINT32 MediaId,
    IN EFI_LBA Lba,
    IN UINTN BufferSize,
    IN VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_FLUSH)(
    IN EFI_BLOCK_IO_PROTOCOL *This);

typedef struct _EFI_BLOCK_IO_PROTOCOL {
    UINT64                  Revision;
    EFI_BLOCK_IO_MEDIA     *Media;
    EFI_BLOCK_RESET         Reset;
    EFI_BLOCK_READ          ReadBlocks;
    EFI_BLOCK_WRITE         WriteBlocks;
    EFI_BLOCK_FLUSH         FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

/* ---- Text IO (used for debug prints) ---- */
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN CHAR16 *String);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET             Reset;
    EFI_TEXT_STRING            OutputString;
    VOID *TestString;
    VOID *QueryMode;
    VOID *SetMode;
    EFI_TEXT_CLEAR             ClearScreen;
    VOID *SetCursorPosition;
    VOID *EnableCursor;
    VOID *Mode;
};

/* ---- Simple Text Input (keyboard) ---- */
typedef struct {
    UINT16  ScanCode;     /* 0x01 up, 0x02 down, 0x03 right, 0x04 left, 0x00 none */
    CHAR16  UnicodeChar;  /* printable char, 0x0D = Enter, 0x20 = Space */
} EFI_INPUT_KEY;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    OUT EFI_INPUT_KEY *Key);

struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET      Reset;
    EFI_INPUT_READ_KEY   ReadKeyStroke;
    VOID                *WaitForKey;
};

/* UEFI scan codes for the arrow keys */
#define SCAN_CODE_UP    0x01
#define SCAN_CODE_DOWN  0x02
#define SCAN_CODE_RIGHT 0x03
#define SCAN_CODE_LEFT  0x04
#define SCAN_CODE_ESC   0x17

/* ---- Boot / Runtime Services (offsets must match EDK2) ---- */
typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress, MaxAllocateType } EFI_ALLOCATE_TYPE;
typedef enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
               EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
               EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
               EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
               EfiPalCode, EfiPersistentMemory, EfiMaxMemoryType } EFI_MEMORY_TYPE;
typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown, EfiResetPlatformSpecific } EFI_RESET_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    IN EFI_MEMORY_TYPE PoolType, IN UINTN Size, OUT VOID **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    IN VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    IN EFI_ALLOCATE_TYPE Type, IN EFI_MEMORY_TYPE MemoryType,
    IN UINTN Pages, IN OUT EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    IN EFI_PHYSICAL_ADDRESS Memory, IN UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_EXIT)(
    IN EFI_HANDLE ImageHandle, IN EFI_STATUS ExitStatus,
    IN UINTN ExitDataSize, IN CHAR16 *ExitData);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(
    IN UINTN Microseconds);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    IN EFI_LOCATE_SEARCH_TYPE SearchType, IN EFI_GUID *Protocol,
    IN VOID *SearchKey, OUT UINTN *NoHandles, OUT EFI_HANDLE **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    IN EFI_GUID *Protocol, IN VOID *Registration, OUT VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_COPY_MEM)(
    IN VOID *Destination, IN VOID *Source, IN UINTN Length);
typedef EFI_STATUS (EFIAPI *EFI_SET_MEM)(
    IN VOID *Buffer, IN UINTN Size, IN UINT8 Value);

typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    IN CHAR16 *VariableName, IN EFI_GUID *VendorGuid,
    OUT UINT32 *Attributes, IN OUT UINTN *DataSize, OUT VOID *Data);
typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(
    IN CHAR16 *VariableName, IN EFI_GUID *VendorGuid,
    IN UINT32 Attributes, IN UINTN DataSize, IN VOID *Data);
typedef EFI_STATUS (EFIAPI *EFI_RESET_SYSTEM)(
    IN EFI_RESET_TYPE ResetType, IN EFI_STATUS ResetStatus,
    IN UINTN DataSize, IN VOID *ResetData);

typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *LowerTPL;
    /* Memory Services (must match the real table order!) */
    EFI_ALLOCATE_PAGES  AllocatePages;
    EFI_FREE_PAGES      FreePages;
    VOID *GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL     FreePool;
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    VOID *LoadImage;
    VOID *StartImage;
    EFI_EXIT Exit;
    VOID *UnloadImage;
    VOID *ExitBootServices;
    VOID *GetNextMonotonicCount;
    EFI_STALL Stall;
    VOID *SetWatchdogTimer;
    VOID *ConnectController;
    VOID *DisconnectController;
    VOID *OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;
    VOID *ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    VOID *InstallMultipleProtocolInterfaces;
    VOID *UninstallMultipleProtocolInterfaces;
    VOID *CalculateCrc32;
    EFI_COPY_MEM CopyMem;
    EFI_SET_MEM   SetMem;
    VOID *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *GetTime;
    VOID *SetTime;
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    VOID *SetVirtualAddressMap;
    VOID *ConvertPointer;
    EFI_GET_VARIABLE  GetVariable;
    VOID *GetNextVariableName;
    EFI_SET_VARIABLE  SetVariable;
    VOID *GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM  ResetSystem;
    VOID *UpdateCapsule;
    VOID *QueryCapsuleCapabilities;
    VOID *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct EFI_CONFIGURATION_TABLE_ {
    EFI_GUID *VendorGuid;
    VOID     *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct EFI_SYSTEM_TABLE_ {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32  FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES    *BootServices;
    UINTN  NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- EFI_SIMPLE_FILE_SYSTEM_PROTOCOL / EFI_FILE_PROTOCOL ----
 * 让固件自带的 FAT 驱动帮我们读写文件，避免手搓 FAT32 簇分配/目录项。
 * 结构体字段顺序必须严格匹配 UEFI 规范（固件按偏移填函数指针）。 */
#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_VOLUME_OPEN)(
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    OUT EFI_FILE_PROTOCOL **Root);
typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    IN EFI_FILE_PROTOCOL *This,
    OUT EFI_FILE_PROTOCOL **NewHandle,
    IN CHAR16 *FileName,
    IN UINT64 OpenMode,
    IN UINT64 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(IN EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_DELETE)(IN EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize, OUT VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(
    IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize, IN VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_POSITION)(
    IN EFI_FILE_PROTOCOL *This, OUT UINT64 *Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(
    IN EFI_FILE_PROTOCOL *This, IN UINT64 Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    IN EFI_FILE_PROTOCOL *This, IN EFI_GUID *InformationType,
    IN OUT UINTN *BufferSize, OUT VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_INFO)(
    IN EFI_FILE_PROTOCOL *This, IN EFI_GUID *InformationType,
    IN UINTN BufferSize, IN VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_FLUSH)(IN EFI_FILE_PROTOCOL *This);

struct _EFI_FILE_PROTOCOL {
    UINT64                Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    EFI_FILE_DELETE       Delete;
    EFI_FILE_READ         Read;
    EFI_FILE_WRITE        Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    EFI_FILE_SET_INFO     SetInfo;
    EFI_FILE_FLUSH        Flush;
};

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64          Revision;
    EFI_VOLUME_OPEN OpenVolume;
};

/* 卷（文件系统）信息查询：GetInfo(EFI_FILE_SYSTEM_INFO_GUID) 返回，
 * 含卷总字节数与剩余字节数，用于"磁盘空间"窗口显示。 */
typedef struct {
    UINT64   Size;          /* 本结构体大小（含卷标） */
    BOOLEAN  ReadOnly;
    UINT64   VolumeSize;    /* 卷总字节数 */
    UINT64   FreeSpace;     /* 剩余可用字节数 */
    CHAR16   VolumeLabel[1];
} EFI_FILE_SYSTEM_INFO;

/* ---- GUIDs (little-endian fields) ---- */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}
#define EFI_SIMPLE_POINTER_PROTOCOL_GUID \
    {0x31878f3b,0xd5b1,0x45e8,{0x8a,0x3c,0x12,0x3f,0x72,0x83,0x64,0x86}}
#define EFI_ABSOLUTE_POINTER_PROTOCOL_GUID \
    {0x8D59D32B,0xC655,0x4AE9,{0x9B,0x15,0xF2,0x59,0x04,0x99,0x2A,0x43}}
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    {0x964E5B21,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_FILE_SYSTEM_INFO_GUID \
    {0x09576e93,0x6d3f,0x11d2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
#define EFI_GLOBAL_VARIABLE_GUID \
    {0x8BE4DF61,0x93CA,0x11d2,{0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}}

/* helper: compare GUIDs */
static inline INTN efi_guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    return (a->Data1==b->Data1 && a->Data2==b->Data2 && a->Data3==b->Data3 &&
            a->Data4[0]==b->Data4[0] && a->Data4[1]==b->Data4[1] &&
            a->Data4[2]==b->Data4[2] && a->Data4[3]==b->Data4[3] &&
            a->Data4[4]==b->Data4[4] && a->Data4[5]==b->Data4[5] &&
            a->Data4[6]==b->Data4[6] && a->Data4[7]==b->Data4[7]);
}

#endif /* HUDOS_EFI_H */
