/*
 * Copyright (c) 2011, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// psiclient.cpp : Defines the entry point for the application.
//

#include "stdafx.h"
#include "psiclient.h"
#include "vpnconnection.h"
#include "webbrowser.h"

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
TCHAR szTitle[MAX_LOADSTRING];                    // The title bar text
TCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR    lpCmdLine,
                     int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

     // TODO: Place code here.
    MSG msg;
    HACCEL hAccelTable;

    // Initialize global strings
    LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadString(hInstance, IDC_PSICLIENT, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_PSICLIENT));

    // Main message loop:
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
//  COMMENTS:
//
//    This function and its usage are only necessary if you want this code
//    to be compatible with Win32 systems prior to the 'RegisterClassEx'
//    function that was added to Windows 95. It is important to call this function
//    so that the application will get 'well formed' small icons associated
//    with it.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style            = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra        = 0;
    wcex.cbWndExtra        = 0;
    wcex.hInstance        = hInstance;
    wcex.hIcon            = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PSICLIENT));
    wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground    = (HBRUSH)(COLOR_WINDOW+1);
    //wcex.lpszMenuName    = MAKEINTRESOURCE(IDC_PSICLIENT);
    wcex.lpszMenuName    = 0;
    wcex.lpszClassName    = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassEx(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//

HWND g_hWnd;

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   HWND hWnd;
   int width = 150, height = 100;
   RECT rect = {0, 0, width, height};

   hInst = hInstance; // Store instance handle in our global variable

   SystemParametersInfo(SPI_GETWORKAREA, 0, &rect, 0);

   hWnd = CreateWindowEx(
            WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
            szWindowClass,
            szTitle,
            WS_OVERLAPPEDWINDOW & ~WS_SYSMENU,
            // CW_USEDEFAULT, 0, CW_USEDEFAULT, 0,
            rect.right - width, rect.bottom - height, width, height,
            NULL, NULL, hInstance, NULL);

   if (!hWnd)
   {
      return FALSE;
   }

   g_hWnd = hWnd;

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

//=== toolbar ========================================================

// http://msdn.microsoft.com/en-us/library/bb760446%28v=VS.85%29.aspx


HWND g_hToolBar = NULL;

HWND CreateToolbar(HWND hWndParent)
{
    // Define some constants.
    const int ImageListID = 0;
    const int numButtons = 3;
    const DWORD buttonStyles = BTNS_AUTOSIZE;
    const int bitmapSize = 32;

    // Create the toolbar.
    HWND hWndToolbar = CreateWindowEx(
                            0, TOOLBARCLASSNAME, NULL, 
                            WS_CHILD | TBSTYLE_WRAPABLE,
                            0, 0, 0, 0,
                            hWndParent, NULL, hInst, NULL);
    if (hWndToolbar == NULL)
    {
        return NULL;
    }

    // Create image list from bitmap

    HIMAGELIST hImageList = ImageList_LoadImage(
        hInst, MAKEINTRESOURCE(IDB_TOOLBAR_ICONS),
        bitmapSize, numButtons, GetSysColor(COLOR_BTNFACE),
        IMAGE_BITMAP, LR_CREATEDIBSECTION);

    // TODO: transparency

    // Set the image list.
    SendMessage(
        hWndToolbar, TB_SETIMAGELIST, (WPARAM)ImageListID, 
        (LPARAM)hImageList);

    // Initialize button info.
    TBBUTTON tbButtons[numButtons] = 
    {
        { MAKELONG(0, ImageListID), IDM_START, TBSTATE_ENABLED, 
          buttonStyles, {0}, 0, (INT_PTR)L"" },
        { MAKELONG(1, ImageListID), IDM_STOP, TBSTATE_ENABLED, 
          buttonStyles, {0}, 0, (INT_PTR)L"" },
        //{ MAKELONG(2, ImageListID), IDM_SHOW_DEBUG_MESSAGES, TBSTATE_ENABLED, 
        //  buttonStyles, {0}, 0, (INT_PTR)L"" },
        //{ MAKELONG(3, ImageListID), IDM_HELP, TBSTATE_ENABLED, 
        //  buttonStyles, {0}, 0, (INT_PTR)L"" },
        { MAKELONG(4, ImageListID), IDM_EXIT, TBSTATE_ENABLED, 
          buttonStyles, {0}, 0, (INT_PTR)L"" },
    };

    // Add buttons.
    SendMessage(
        hWndToolbar, TB_BUTTONSTRUCTSIZE, 
        (WPARAM)sizeof(TBBUTTON), 0);
    SendMessage(
        hWndToolbar, TB_ADDBUTTONS, (WPARAM)numButtons, 
        (LPARAM)&tbButtons);

    // Tell the toolbar to resize itself, and show it.
    SendMessage(hWndToolbar, TB_AUTOSIZE, 0, 0); 
    ShowWindow(hWndToolbar, TRUE);
    return hWndToolbar;
}

//=== my_print ========================================================

HWND g_hListBox = NULL;
bool g_bShowDebugMessages = false;

void my_print(bool bDebugMessage, const TCHAR* format, ...)
{
    if (!bDebugMessage || g_bShowDebugMessages)
    {
        TCHAR* debug_prefix = _T("DEBUG: ");
        TCHAR* buffer = NULL;
        int len;
        va_list args;
        va_start(args, format);
        len = _vsctprintf(format, args) + 1;
        if (bDebugMessage)
        {
            len += _tcsclen(debug_prefix) + 1;
        }
        buffer = (TCHAR*)malloc(len*sizeof(TCHAR));
        if (!buffer) return;
        if (bDebugMessage)
        {
            _tcscpy(buffer, debug_prefix);
            _vstprintf(buffer + _tcsclen(debug_prefix), format, args);
        }
        else
        {
            _vstprintf(buffer, format, args);
        }
        va_end(args);
        SendMessage(g_hListBox, LB_ADDSTRING, NULL, (LPARAM)buffer);
        free(buffer);
        SendMessage(g_hListBox, LB_SETCURSEL,
            SendMessage(g_hListBox, LB_GETCOUNT, NULL, NULL)-1, NULL);
    }
}

//=== other stuff ========================================================

WebBrowser g_webBrowser;
VPNConnection g_vpnConnection;

void Start()
{
    // Configure the proxy settings
    if (!g_vpnConnection.Establish())
    {
        my_print(false, _T("error configuring proxy settings"));
    }
    else
    {
        // Start web browser
        g_webBrowser.Open();

        my_print(false, _T("PsiphonY started"));
    }
}

void Stop()
{
    // Stop web browser
    g_webBrowser.Close();

    // Reset the proxy settings
    g_vpnConnection.Remove();
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND    - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY    - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int wmId, wmEvent;
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rect;
    HGDIOBJ font;
    int toolbar_height = 48; // TODO: calculate

    switch (message)
    {
    case WM_CREATE:

        g_hToolBar = CreateToolbar(hWnd);

        g_hListBox = CreateWindow(_T("listbox"),
                                _T(""),
                                WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_DISABLENOSCROLL|LBS_NOTIFY,
                                0, 0, 1, 1,
                                hWnd, NULL, NULL, NULL);
        font = GetStockObject(DEFAULT_GUI_FONT);
        SendMessage(g_hListBox, WM_SETFONT, (WPARAM)font, NULL);

        break;
    case WM_SIZE:
        // make list box fill window client area
        GetClientRect(hWnd, &rect);
        if (g_hToolBar != NULL)
        {
            MoveWindow(
                g_hToolBar,
                0, 0,
                rect.right-rect.left, toolbar_height,
                TRUE);
        }
        if (g_hListBox != NULL)
        {
            MoveWindow(
                g_hListBox,
                0, toolbar_height,
                rect.right-rect.left, rect.bottom-rect.top - toolbar_height,
                TRUE);
        }
        break;
    case WM_COMMAND:

        wmId    = LOWORD(wParam);
        wmEvent = HIWORD(wParam);

        // Parse the menu selections:
        switch (wmId)
        {
        case IDM_START:
            Start();
            break;
        case IDM_STOP:
            Stop();
            break;
        case IDM_SHOW_DEBUG_MESSAGES:
            g_bShowDebugMessages = !g_bShowDebugMessages;
            my_print(false, _T("Show debug messages: %s"), g_bShowDebugMessages ? _T("Yes") : _T("No"));
            break;
        // TODO: remove about and exit?  The menu is currently hidden
        case IDM_HELP:
            // TODO: help?
            break;
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    case WM_PAINT:
        hdc = BeginPaint(hWnd, &ps);
        // TODO: Add any drawing code here...
        EndPaint(hWnd, &ps);
        break;
    case WM_DESTROY:
        Stop();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
