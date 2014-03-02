/*
 * COPYRIGHT:       GPL - See COPYING in the top level directory
 * PROJECT:         ReactOS Virtual DOS Machine
 * FILE:            ntvdm.c
 * PURPOSE:         Virtual DOS Machine
 * PROGRAMMERS:     Aleksandar Andrejevic <theflash AT sdf DOT lonestar DOT org>
 */

/* INCLUDES *******************************************************************/

#define NDEBUG

#include "ntvdm.h"
#include "emulator.h"

#include "clock.h"
#include "hardware/ps2.h"
#include "hardware/vga.h"
#include "bios/bios.h"
#include "dos/dem.h"

#include "resource.h"

/*
 * Activate this line if you want to run NTVDM in standalone mode with:
 * ntvdm.exe <program>
 */
#define STANDALONE

/* VARIABLES ******************************************************************/

static HANDLE ConsoleInput  = INVALID_HANDLE_VALUE;
static HANDLE ConsoleOutput = INVALID_HANDLE_VALUE;
static DWORD  OrgConsoleInputMode, OrgConsoleOutputMode;
static CONSOLE_CURSOR_INFO         OrgConsoleCursorInfo;
static CONSOLE_SCREEN_BUFFER_INFO  OrgConsoleBufferInfo;

static HMENU hConsoleMenu  = NULL;
static INT   VdmMenuPos    = -1;
static BOOLEAN ShowPointer = FALSE;

/*
 * Those menu helpers were taken from the GUI frontend in winsrv.dll
 */
typedef struct _VDM_MENUITEM
{
    UINT uID;
    const struct _VDM_MENUITEM *SubMenu;
    WORD wCmdID;
} VDM_MENUITEM, *PVDM_MENUITEM;

static const VDM_MENUITEM VdmMenuItems[] =
{
    { IDS_VDM_QUIT, NULL, ID_VDM_QUIT },

    { 0, NULL, 0 }      /* End of list */
};

static const VDM_MENUITEM VdmMainMenuItems[] =
{
    { -1, NULL, 0 },    /* Separator */
    { IDS_HIDE_MOUSE,   NULL, ID_SHOWHIDE_MOUSE },  /* Hide mouse; can be renamed to Show mouse */
    { IDS_VDM_MENU  ,   VdmMenuItems,         0 },  /* ReactOS VDM Menu */

    { 0, NULL, 0 }      /* End of list */
};

static VOID
AppendMenuItems(HMENU hMenu,
                const VDM_MENUITEM *Items)
{
    UINT i = 0;
    WCHAR szMenuString[255];
    HMENU hSubMenu;

    do
    {
        if (Items[i].uID != (UINT)-1)
        {
            if (LoadStringW(GetModuleHandle(NULL),
                            Items[i].uID,
                            szMenuString,
                            sizeof(szMenuString) / sizeof(szMenuString[0])) > 0)
            {
                if (Items[i].SubMenu != NULL)
                {
                    hSubMenu = CreatePopupMenu();
                    if (hSubMenu != NULL)
                    {
                        AppendMenuItems(hSubMenu, Items[i].SubMenu);

                        if (!AppendMenuW(hMenu,
                                         MF_STRING | MF_POPUP,
                                         (UINT_PTR)hSubMenu,
                                         szMenuString))
                        {
                            DestroyMenu(hSubMenu);
                        }
                    }
                }
                else
                {
                    AppendMenuW(hMenu,
                                MF_STRING,
                                Items[i].wCmdID,
                                szMenuString);
                }
            }
        }
        else
        {
            AppendMenuW(hMenu,
                        MF_SEPARATOR,
                        0,
                        NULL);
        }
        i++;
    } while (!(Items[i].uID == 0 && Items[i].SubMenu == NULL && Items[i].wCmdID == 0));
}

static VOID
CreateVdmMenu(HANDLE ConOutHandle)
{
    hConsoleMenu = ConsoleMenuControl(ConsoleOutput,
                                      ID_SHOWHIDE_MOUSE,
                                      ID_VDM_QUIT);
    if (hConsoleMenu == NULL) return;

    VdmMenuPos = GetMenuItemCount(hConsoleMenu);
    AppendMenuItems(hConsoleMenu, VdmMainMenuItems);
    DrawMenuBar(GetConsoleWindow());
}

static VOID
DestroyVdmMenu(VOID)
{
    UINT i = 0;
    const VDM_MENUITEM *Items = VdmMainMenuItems;

    do
    {
        DeleteMenu(hConsoleMenu, VdmMenuPos, MF_BYPOSITION);
        i++;
    } while (!(Items[i].uID == 0 && Items[i].SubMenu == NULL && Items[i].wCmdID == 0));

    DrawMenuBar(GetConsoleWindow());
}

static VOID ShowHideMousePointer(HANDLE ConOutHandle, BOOLEAN ShowPtr)
{
    WCHAR szMenuString[255] = L"";

    if (ShowPtr)
    {
        /* Be sure the cursor will be shown */
        while (ShowConsoleCursor(ConOutHandle, TRUE) < 0) ;
    }
    else
    {
        /* Be sure the cursor will be hidden */
        while (ShowConsoleCursor(ConOutHandle, FALSE) >= 0) ;
    }

    if (LoadStringW(GetModuleHandle(NULL),
                    (!ShowPtr ? IDS_SHOW_MOUSE : IDS_HIDE_MOUSE),
                    szMenuString,
                    sizeof(szMenuString) / sizeof(szMenuString[0])) > 0)
    {
        ModifyMenu(hConsoleMenu, ID_SHOWHIDE_MOUSE,
                   MF_BYCOMMAND, ID_SHOWHIDE_MOUSE, szMenuString);
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

VOID DisplayMessage(LPCWSTR Format, ...)
{
    WCHAR Buffer[256];
    va_list Parameters;

    va_start(Parameters, Format);
    _vsnwprintf(Buffer, 256, Format, Parameters);
    DPRINT1("\n\nNTVDM Subsystem\n%S\n\n", Buffer);
    MessageBoxW(NULL, Buffer, L"NTVDM Subsystem", MB_OK);
    va_end(Parameters);
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ControlType)
{
    switch (ControlType)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        {
            /* Call INT 23h */
            EmulatorInterrupt(0x23);
            break;
        }
        default:
        {
            /* Stop the VDM if the user logs out or closes the console */
            EmulatorTerminate();
        }
    }
    return TRUE;
}

VOID ConsoleInitUI(VOID)
{
    CreateVdmMenu(ConsoleOutput);
}

VOID ConsoleCleanupUI(VOID)
{
    /* Display again properly the mouse pointer */
    if (ShowPointer) ShowHideMousePointer(ConsoleOutput, ShowPointer);

    DestroyVdmMenu();
}

DWORD WINAPI PumpConsoleInput(LPVOID Parameter)
{
    HANDLE ConsoleInput = (HANDLE)Parameter;
    INPUT_RECORD InputRecord;
    DWORD Count;

    while (VdmRunning)
    {
        /* Wait for an input record */
        if (!ReadConsoleInput(ConsoleInput, &InputRecord, 1, &Count))
        {
            DWORD LastError = GetLastError();
            DPRINT1("Error reading console input (0x%p, %lu) - Error %lu\n", ConsoleInput, Count, LastError);
            return LastError;
        }

        ASSERT(Count != 0);

        /* Check the event type */
        switch (InputRecord.EventType)
        {
            case KEY_EVENT:
            case MOUSE_EVENT:
                /* Send it to the PS/2 controller */
                PS2Dispatch(&InputRecord);
                break;

            case MENU_EVENT:
            {
                switch (InputRecord.Event.MenuEvent.dwCommandId)
                {
                    case ID_SHOWHIDE_MOUSE:
                        ShowHideMousePointer(ConsoleOutput, ShowPointer);
                        ShowPointer = !ShowPointer;
                        break;

                    case ID_VDM_QUIT:
                        /* Stop the VDM */
                        EmulatorTerminate();
                        break;

                    default:
                        break;
                }

                break;
            }

            default:
                break;
        }
    }

    return 0;
}

BOOL ConsoleInit(VOID)
{
    /* Set the handler routine */
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    /* Get the input handle to the real console, and check for success */
    ConsoleInput = CreateFileW(L"CONIN$",
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL,
                               OPEN_EXISTING,
                               0,
                               NULL);
    if (ConsoleInput == INVALID_HANDLE_VALUE)
    {
        wprintf(L"FATAL: Cannot retrieve a handle to the console input\n");
        return FALSE;
    }

    /* Get the output handle to the real console, and check for success */
    ConsoleOutput = CreateFileW(L"CONOUT$",
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_EXISTING,
                                0,
                                NULL);
    if (ConsoleOutput == INVALID_HANDLE_VALUE)
    {
        CloseHandle(ConsoleInput);
        wprintf(L"FATAL: Cannot retrieve a handle to the console output\n");
        return FALSE;
    }

    /* Save the original input and output console modes */
    if (!GetConsoleMode(ConsoleInput , &OrgConsoleInputMode ) ||
        !GetConsoleMode(ConsoleOutput, &OrgConsoleOutputMode))
    {
        CloseHandle(ConsoleOutput);
        CloseHandle(ConsoleInput);
        wprintf(L"FATAL: Cannot save console in/out modes\n");
        return FALSE;
    }

    /* Save the original cursor and console screen buffer information */
    if (!GetConsoleCursorInfo(ConsoleOutput, &OrgConsoleCursorInfo) ||
        !GetConsoleScreenBufferInfo(ConsoleOutput, &OrgConsoleBufferInfo))
    {
        CloseHandle(ConsoleOutput);
        CloseHandle(ConsoleInput);
        wprintf(L"FATAL: Cannot save console cursor/screen-buffer info\n");
        return FALSE;
    }

    /* Initialize the UI */
    ConsoleInitUI();

    return TRUE;
}

VOID ConsoleCleanup(VOID)
{
    SMALL_RECT ConRect;
    CONSOLE_SCREEN_BUFFER_INFO ConsoleInfo;

    /* Restore the old screen buffer */
    SetConsoleActiveScreenBuffer(ConsoleOutput);

    /* Restore the original console size */
    GetConsoleScreenBufferInfo(ConsoleOutput, &ConsoleInfo);
    ConRect.Left = 0; // OrgConsoleBufferInfo.srWindow.Left;
    // ConRect.Top  = ConsoleInfo.dwCursorPosition.Y / (OrgConsoleBufferInfo.srWindow.Bottom - OrgConsoleBufferInfo.srWindow.Top + 1);
    // ConRect.Top *= (OrgConsoleBufferInfo.srWindow.Bottom - OrgConsoleBufferInfo.srWindow.Top + 1);
    ConRect.Top    = ConsoleInfo.dwCursorPosition.Y;
    ConRect.Right  = ConRect.Left + OrgConsoleBufferInfo.srWindow.Right  - OrgConsoleBufferInfo.srWindow.Left;
    ConRect.Bottom = ConRect.Top  + OrgConsoleBufferInfo.srWindow.Bottom - OrgConsoleBufferInfo.srWindow.Top ;
    /* See the following trick explanation in vga.c:VgaEnterTextMode() */
    SetConsoleScreenBufferSize(ConsoleOutput, OrgConsoleBufferInfo.dwSize);
    SetConsoleWindowInfo(ConsoleOutput, TRUE, &ConRect);
    // SetConsoleWindowInfo(ConsoleOutput, TRUE, &OrgConsoleBufferInfo.srWindow);
    SetConsoleScreenBufferSize(ConsoleOutput, OrgConsoleBufferInfo.dwSize);

    /* Restore the original cursor shape */
    SetConsoleCursorInfo(ConsoleOutput, &OrgConsoleCursorInfo);

    /* Restore the original input and output console modes */
    SetConsoleMode(ConsoleOutput, OrgConsoleOutputMode);
    SetConsoleMode(ConsoleInput , OrgConsoleInputMode );

    /* Cleanup the UI */
    ConsoleCleanupUI();

    /* Close the console handles */
    if (ConsoleOutput != INVALID_HANDLE_VALUE) CloseHandle(ConsoleOutput);
    if (ConsoleInput  != INVALID_HANDLE_VALUE) CloseHandle(ConsoleInput);
}

INT wmain(INT argc, WCHAR *argv[])
{
#ifndef STANDALONE
    wprintf(L"\nReactOS Virtual DOS Machine\n\n"
            L"OS integration (BaseVDM) unimplemented\n");
    return 0;
#else

    CHAR CommandLine[DOS_CMDLINE_LENGTH];

    if (argc == 2 && argv[1] != NULL)
    {
        WideCharToMultiByte(CP_ACP, 0, argv[1], -1, CommandLine, sizeof(CommandLine), NULL, NULL);
    }
    else
    {
        wprintf(L"\nReactOS Virtual DOS Machine\n\n"
                L"Usage: NTVDM <executable>\n");
        return 0;
    }

    DPRINT1("\n\n\nNTVDM - Starting '%s'...\n\n\n", CommandLine);

    /* Initialize the console */
    if (!ConsoleInit())
    {
        wprintf(L"FATAL: A problem occurred when trying to initialize the console\n");
        goto Cleanup;
    }

    /* Initialize the emulator */
    if (!EmulatorInitialize(ConsoleInput, ConsoleOutput))
    {
        wprintf(L"FATAL: Failed to initialize the emulator\n");
        goto Cleanup;
    }

    /* Initialize the system BIOS */
    if (!BiosInitialize(NULL))
    {
        wprintf(L"FATAL: Failed to initialize the VDM BIOS.\n");
        goto Cleanup;
    }

    /* Initialize the VDM DOS kernel */
    if (!DosInitialize(NULL))
    {
        wprintf(L"FATAL: Failed to initialize the VDM DOS kernel.\n");
        goto Cleanup;
    }

    /* Start the process from the command line */
    if (!DosCreateProcess(CommandLine, 0))
    {
        DisplayMessage(L"Could not start program: %S", CommandLine);
        goto Cleanup;
    }

    /* Start simulation */
    EmulatorSimulate();

    /* Perform another screen refresh */
    VgaRefreshDisplay();

Cleanup:
    BiosCleanup();
    EmulatorCleanup();
    ConsoleCleanup();

    DPRINT1("\n\n\nNTVDM - Exiting...\n\n\n");

    return 0;

#endif
}

/* EOF */
