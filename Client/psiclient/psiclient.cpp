/*
 * Copyright (c) 2015, Psiphon Inc.
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

 //==== Includes ===============================================================

#include "stdafx.h"

// This is for COM functions
# pragma comment(lib, "wbemuuid.lib")

#include "psiclient.h"
#include "logging.h"
#include <mCtrl/html.h>
#include "connectionmanager.h"
#include "embeddedvalues.h"
#include "transport.h"
#include "config.h"
#include "usersettings.h"
#include "utilities.h"
#include "webbrowser.h"
#include "limitsingleinstance.h"
#include "htmldlg.h"
#include "stopsignal.h"
#include "diagnostic_info.h"
#include "systemproxysettings.h"

//==== Globals ================================================================

#define MAX_LOADSTRING 100

HINSTANCE g_hInst;
TCHAR g_szTitle[MAX_LOADSTRING];
TCHAR g_szWindowClass[MAX_LOADSTRING];

HWND g_hWnd = NULL;
ConnectionManager g_connectionManager;

LimitSingleInstance g_singleInstanceObject(TEXT("Global\\{B88F6262-9CC8-44EF-887D-FB77DC89BB8C}"));

static HWND g_hHtmlCtrl = NULL;
static bool g_htmlUiReady = false;
// The HTML control has a bad habit of sending messages after we've posted WM_QUIT,
// which leads to a crash on exit.
static bool g_htmlUiFinished = false;

//==== Controls ================================================================

static void OnResize(HWND hWnd, UINT uWidth, UINT uHeight)
{
    SetWindowPos(g_hHtmlCtrl, NULL, 0, 0, uWidth, uHeight, SWP_NOZORDER);
}

void OnCreate(HWND hWndParent)
{
    Json::Value initJSON, settingsJSON;
    Settings::ToJson(settingsJSON);
    initJSON["Settings"] = settingsJSON;
    initJSON["Cookies"] = Settings::GetCookies();
    initJSON["Config"] = Json::Value();
    initJSON["Config"]["Language"] = TStringToNarrow(GetLocaleName());
    initJSON["Config"]["Banner"] = "banner.png";
    initJSON["Config"]["Version"] = CLIENT_VERSION;
    initJSON["Config"]["InfoURL"] = TStringToNarrow(INFO_LINK_URL);
    initJSON["Config"]["NewVersionEmail"] = GET_NEW_VERSION_EMAIL;
    initJSON["Config"]["NewVersionURL"] = GET_NEW_VERSION_URL;
    initJSON["Config"]["FaqURL"] = FAQ_URL;
    initJSON["Config"]["DataCollectionInfoURL"] = DATA_COLLECTION_INFO_URL;
#ifdef _DEBUG
    initJSON["Config"]["Debug"] = true;
#else
    initJSON["Config"]["Debug"] = false;
#endif

    Json::FastWriter jsonWriter;
    tstring initJsonString = NarrowToTString(jsonWriter.write(initJSON));

    tstring url = ResourceToUrl(_T("main.html"), NULL, initJsonString.c_str());

    /* Create the html control */
    g_hHtmlCtrl = CreateWindow(
        MC_WC_HTML,
        url.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        MC_HS_NOCONTEXTMENU |   // don't show context menu
        MC_HS_NOTIFYNAV,        // notify owner window on navigation attempts
        0, 0, 0, 0,
        hWndParent,
        (HMENU)IDC_HTML_CTRL,
        g_hInst,
        NULL);
}


//==== String Table helpers ==================================================

#define STRING_KEY_STATE_STOPPED_TITLE          "appbackend#state-stopped-title"
#define STRING_KEY_STATE_STOPPED_BODY           "appbackend#state-stopped-body"
#define STRING_KEY_STATE_STARTING_TITLE         "appbackend#state-starting-title"
#define STRING_KEY_STATE_STARTING_BODY          "appbackend#state-starting-body"
#define STRING_KEY_STATE_CONNECTED_TITLE        "appbackend#state-connected-title"
#define STRING_KEY_STATE_CONNECTED_BODY         "appbackend#state-connected-body"
#define STRING_KEY_STATE_STOPPING_TITLE         "appbackend#state-stopping-title"
#define STRING_KEY_STATE_STOPPING_BODY          "appbackend#state-stopping-body"
#define STRING_KEY_MINIMIZED_TO_SYSTRAY_TITLE   "appbackend#minimized-to-systray-title"
#define STRING_KEY_MINIMIZED_TO_SYSTRAY_BODY    "appbackend#minimized-to-systray-body"

static map<string, wstring> g_stringTable;

static void AddStringTableEntry(const string& entryJson)
{
    Json::Value json;
    Json::Reader reader;
    bool parsingSuccessful = reader.parse(entryJson, json);
    if (!parsingSuccessful)
    {
        my_print(NOT_SENSITIVE, true, _T("%s:%d: Failed to parse string table entry"), __TFUNCTION__, __LINE__);
        return;
    }

    string key, narrowStr;

    try
    {
        if (!json.isMember("key") ||
            !json.isMember("string"))
        {
            // The stored values are invalid
            return;
        }

        key = json.get("key", "").asString();
        narrowStr = json.get("string", "").asString();
        if (key.empty() || narrowStr.empty())
        {
            return;
        }
    }
    catch (exception& e)
    {
        my_print(NOT_SENSITIVE, false, _T("%s:%d: JSON parse exception: %S"), __TFUNCTION__, __LINE__, e.what());
        return;
    }

    wstring str = UTF8ToWString(narrowStr.c_str());

    g_stringTable[key] = str;
}

// Returns true if the string table entry is found, false otherwise. 
static bool GetStringTableEntry(LPSTR key, wstring& o_entry)
{
    o_entry.clear();

    map<string, wstring>::const_iterator iter = g_stringTable.find(key);
    if (iter == g_stringTable.end())
    {
        return false;
    }

    o_entry = iter->second;

    return true;
}


//==== Systray/Notification helpers ===========================================

// General info on Notifications: https://msdn.microsoft.com/en-us/library/ee330740%28v=vs.85%29.aspx
// NOTIFYICONDATA: https://msdn.microsoft.com/en-us/library/bb773352%28v=vs.85%29.aspx
// Shell_NotifyIcon: https://msdn.microsoft.com/en-us/library/bb762159%28VS.85%29.aspx

// Not defined on older OSes
#ifndef NIIF_LARGE_ICON
#define NIIF_LARGE_ICON 0x00000020
#endif

static NOTIFYICONDATA g_notifyIconData = { 0 };
static HICON g_notifyIconStopped = NULL;
static HICON g_notifyIconConnected = NULL;
static bool g_notifyIconInitialized = false;


// InitSystrayIcon initializes the systray icon/notification. Gets called by 
// UpdateSystrayState and should not be called directly.
static bool InitSystrayIcon() {
    if (g_notifyIconInitialized)
    {
        return true;
    }

    g_notifyIconStopped = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_SYSTRAY_STOPPED));
    g_notifyIconConnected = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_SYSTRAY_CONNECTED));

    g_notifyIconData.cbSize = sizeof(NOTIFYICONDATA);
    g_notifyIconData.hWnd = g_hWnd;
    g_notifyIconData.uID = 1;  // Used to identify multiple icons. We only use one.
    g_notifyIconData.uCallbackMessage = WM_PSIPHON_TRAY_ICON_NOTIFY;
    g_notifyIconData.uTimeout = 30000;  // 30s is the max time the balloon can show

    g_notifyIconData.hIcon = g_notifyIconStopped;  // Begin with the stopped icon.

    g_notifyIconData.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;

    _tcsncpy_s(
        g_notifyIconData.szTip,
        sizeof(g_notifyIconData.szTip) / sizeof(TCHAR),
        g_szTitle,
        _TRUNCATE);

    g_notifyIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;

    BOOL bSuccess = Shell_NotifyIcon(NIM_ADD, &g_notifyIconData);
    if (bSuccess)
    {
        g_notifyIconData.uVersion = NOTIFYICON_VERSION;
        (void)Shell_NotifyIcon(NIM_SETVERSION, &g_notifyIconData);
    }

    g_notifyIconInitialized = !!bSuccess;
    return g_notifyIconInitialized;
}

// UpdateSystrayIcon sets the current systray icon. 
// If infoTitle is non-empty, then it will also display a balloon.
// If hIcon is NULL, then the icon will not be changed.
static void UpdateSystrayIcon(HICON hIcon, const wstring& infoTitle, const wstring& infoBody)
{
    if (!InitSystrayIcon() || g_htmlUiFinished)
    {
        return;
    }

    // The body isn't allowed to be an empty string, so set it to a space.
    wstring infoBodyToUse = infoBody.empty() ? L" " : infoBody;

    if (!infoTitle.empty())
    {
        _tcsncpy_s(
            g_notifyIconData.szInfoTitle,
            sizeof(g_notifyIconData.szInfoTitle) / sizeof(TCHAR),
            infoTitle.c_str(),
            _TRUNCATE);

        _tcsncpy_s(
            g_notifyIconData.szInfo,
            sizeof(g_notifyIconData.szInfo) / sizeof(TCHAR),
            infoBody.c_str(),
            _TRUNCATE);

        g_notifyIconData.uFlags |= NIF_INFO;
    }
    else
    {
        // We don't have the info text (yet)
        g_notifyIconData.uFlags &= ~NIF_INFO;
    }

    if (hIcon != NULL)
    {
        g_notifyIconData.hIcon = hIcon;
    }

    (void)Shell_NotifyIcon(NIM_MODIFY, &g_notifyIconData);
}

// SystrayIconCleanup must be called when the application is exiting.
static void SystrayIconCleanup()
{
    (void)Shell_NotifyIcon(NIM_DELETE, &g_notifyIconData);
    g_notifyIconInitialized = false;
}

// If we start using more timers, we should move the IDs to a single location 
// where they can be kept track of.
#define TIMER_ID_HIDE   100

static VOID CALLBACK HandleMinimizeHelper(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    KillTimer(g_hWnd, TIMER_ID_HIDE);
    
    if (g_notifyIconInitialized)
    {
        ShowWindow(g_hWnd, SW_HIDE);

        // Show a balloon letting the user know where the app went
        wstring infoTitle, infoBody;
        (void)GetStringTableEntry(STRING_KEY_MINIMIZED_TO_SYSTRAY_TITLE, infoTitle);
        (void)GetStringTableEntry(STRING_KEY_MINIMIZED_TO_SYSTRAY_BODY, infoBody);

        UpdateSystrayIcon(NULL, infoTitle, infoBody);
    }
}

static void HandleMinimize()
{
    // The time on this is a rough guess at how long the minimize animation will take.
    SetTimer(g_hWnd, TIMER_ID_HIDE, 300, HandleMinimizeHelper);
}

// UpdateSystrayConnectedState must be called every time the application connected state
// changes.
static void UpdateSystrayConnectedState()
{
    if (g_htmlUiFinished)
    {
        return;
    }

    wstring infoTitle, infoBody;
    bool infoTitleFound = false, infoBodyFound = false;
    HICON hIcon = NULL;

    if (g_connectionManager.GetState() == CONNECTION_MANAGER_STATE_CONNECTED)
    {
        hIcon = g_notifyIconConnected;
        infoTitleFound = GetStringTableEntry(STRING_KEY_STATE_CONNECTED_TITLE, infoTitle);
        infoBodyFound = GetStringTableEntry(STRING_KEY_STATE_CONNECTED_BODY, infoBody);
    }
    else if (g_connectionManager.GetState() == CONNECTION_MANAGER_STATE_STARTING)
    {
        hIcon = g_notifyIconConnected;
        infoTitleFound = GetStringTableEntry(STRING_KEY_STATE_STARTING_TITLE, infoTitle);
        infoBodyFound = GetStringTableEntry(STRING_KEY_STATE_STARTING_BODY, infoBody);
    }
    else if (g_connectionManager.GetState() == CONNECTION_MANAGER_STATE_STOPPING)
    {
        hIcon = g_notifyIconStopped;
        infoTitleFound = GetStringTableEntry(STRING_KEY_STATE_STOPPING_TITLE, infoTitle);
        infoBodyFound = GetStringTableEntry(STRING_KEY_STATE_STOPPING_BODY, infoBody);
    }
    else  // CONNECTION_MANAGER_STATE_STOPPED
    {
        hIcon = g_notifyIconStopped;
        infoTitleFound = GetStringTableEntry(STRING_KEY_STATE_STOPPED_TITLE, infoTitle);
        infoBodyFound = GetStringTableEntry(STRING_KEY_STATE_STOPPED_BODY, infoBody);
    }

    UpdateSystrayIcon(hIcon, infoTitle, infoBody);
}


//==== HTML UI helpers ========================================================

// Many of these helpers (particularly the ones that don't need an immediate
// response from the page script) come in pairs: one function to receive the
// arguments, create a buffer, and post a message; and one function to receive
// the posted message and actually do the work.
// We do this so that we won't end up deadlocked between message handling and
// background stuff. For example, the Stop button in the HTML will block the
// page script until the AppLink is processed; but if ConnectionManager.Stop()
// is called directly, then it will wait for the connection thread to die, but
// that thread calls ConnectionManager.SetState(), which calls HtmlUI_SetState(),
// which tries to talk to the page script, but it can't, because the page script
// is blocked!
// So, we're going to PostMessages to ourself whenever possible.

#define WM_PSIPHON_HTMLUI_BEFORENAVIGATE    WM_USER + 200
#define WM_PSIPHON_HTMLUI_SETSTATE          WM_USER + 201
#define WM_PSIPHON_HTMLUI_ADDMESSAGE        WM_USER + 202
#define WM_PSIPHON_HTMLUI_ADDNOTICE         WM_USER + 203
#define WM_PSIPHON_HTMLUI_REFRESHSETTINGS   WM_USER + 204

static void HtmlUI_AddMessage(int priority, LPCTSTR message)
{
    Json::Value json;
    json["priority"] = priority;
    json["message"] = WStringToUTF8(message);
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());

    size_t bufLen = wJson.length() + 1;
    wchar_t* buf = new wchar_t[bufLen];
    wcsncpy_s(buf, bufLen, wJson.c_str(), bufLen);
    buf[bufLen - 1] = L'\0';
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_ADDMESSAGE, (WPARAM)buf, 0);
}

static void HtmlUI_AddMessageHandler(LPCWSTR json)
{
    if (!g_htmlUiReady)
    {
        delete[] json;
        return;
    }

    MC_HMCALLSCRIPTFUNC argStruct = { 0 };
    argStruct.cbSize = sizeof(MC_HMCALLSCRIPTFUNC);
    argStruct.cArgs = 1;
    argStruct.pszArg1 = json;
    if (!SendMessage(
        g_hHtmlCtrl, MC_HM_CALLSCRIPTFUNC,
        (WPARAM)_T("HtmlCtrlInterface_AddMessage"), (LPARAM)&argStruct))
    {
        throw std::exception("UI: HtmlCtrlInterface_AddMessage not found");
    }
    delete[] json;
}

static void HtmlUI_SetState(const wstring& json)
{
    size_t bufLen = json.length() + 1;
    wchar_t* buf = new wchar_t[bufLen];
    wcsncpy_s(buf, bufLen, json.c_str(), bufLen);
    buf[bufLen - 1] = L'\0';
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_SETSTATE, (WPARAM)buf, 0);
}

static void HtmlUI_SetStateHandler(LPCWSTR json)
{
    if (!g_htmlUiReady)
    {
        delete[] json;
        return;
    }

    MC_HMCALLSCRIPTFUNC argStruct = { 0 };
    argStruct.cbSize = sizeof(MC_HMCALLSCRIPTFUNC);
    argStruct.cArgs = 1;
    argStruct.pszArg1 = json;
    if (!SendMessage(
        g_hHtmlCtrl, MC_HM_CALLSCRIPTFUNC,
        (WPARAM)_T("HtmlCtrlInterface_SetState"), (LPARAM)&argStruct))
    {
        throw std::exception("UI: HtmlCtrlInterface_SetState not found");
    }
    delete[] json;
}

static void HtmlUI_AddNotice(const string& noticeJSON)
{
    wstring wJson = UTF8ToWString(noticeJSON.c_str());

    size_t bufLen = wJson.length() + 1;
    wchar_t* buf = new wchar_t[bufLen];
    wcsncpy_s(buf, bufLen, wJson.c_str(), bufLen);
    buf[bufLen - 1] = L'\0';
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_ADDNOTICE, (WPARAM)buf, 0);
}

static void HtmlUI_AddNoticeHandler(LPCWSTR json)
{
    if (!g_htmlUiReady)
    {
        delete[] json;
        return;
    }

    MC_HMCALLSCRIPTFUNC argStruct = { 0 };
    argStruct.cbSize = sizeof(MC_HMCALLSCRIPTFUNC);
    argStruct.cArgs = 1;
    argStruct.pszArg1 = json;
    if (!SendMessage(
        g_hHtmlCtrl, MC_HM_CALLSCRIPTFUNC,
        (WPARAM)_T("HtmlCtrlInterface_AddNotice"), (LPARAM)&argStruct))
    {
        throw std::exception("UI: HtmlCtrlInterface_AddNotice not found");
    }
    delete[] json;
}

static void HtmlUI_RefreshSettings(const string& settingsJSON)
{
    wstring wJson = UTF8ToWString(settingsJSON.c_str());

    size_t bufLen = wJson.length() + 1;
    wchar_t* buf = new wchar_t[bufLen];
    wcsncpy_s(buf, bufLen, wJson.c_str(), bufLen);
    buf[bufLen - 1] = L'\0';
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_REFRESHSETTINGS, (WPARAM)buf, 0);
}

static void HtmlUI_RefreshSettingsHandler(LPCWSTR json)
{
    if (!g_htmlUiReady)
    {
        delete[] json;
        return;
    }

    MC_HMCALLSCRIPTFUNC argStruct = { 0 };
    argStruct.cbSize = sizeof(MC_HMCALLSCRIPTFUNC);
    argStruct.cArgs = 1;
    argStruct.pszArg1 = json;
    if (!SendMessage(
        g_hHtmlCtrl, MC_HM_CALLSCRIPTFUNC,
        (WPARAM)_T("HtmlCtrlInterface_RefreshSettings"), (LPARAM)&argStruct))
    {
        throw std::exception("UI: HtmlCtrlInterface_RefreshSettings not found");
    }
    delete[] json;
}

static void HtmlUI_BeforeNavigate(MC_NMHTMLURL* nmHtmlUrl)
{
    size_t bufLen = _tcslen(nmHtmlUrl->pszUrl) + 1;
    TCHAR* buf = new TCHAR[bufLen];
    _tcsncpy_s(buf, bufLen, nmHtmlUrl->pszUrl, bufLen);
    buf[bufLen - 1] = _T('\0');
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_BEFORENAVIGATE, (WPARAM)buf, 0);
}

#define PSIPHON_LINK_PREFIX     _T("psi:")
static void HtmlUI_BeforeNavigateHandler(LPCTSTR url)
{
    // NOTE: Incoming query parameters will be URI-encoded

    const LPCTSTR appReady = PSIPHON_LINK_PREFIX _T("ready");
    const LPCTSTR appStringTable = PSIPHON_LINK_PREFIX _T("stringtable?");
    const size_t appStringTableLen = _tcslen(appStringTable);
    const LPCTSTR appStart = PSIPHON_LINK_PREFIX _T("start");
    const LPCTSTR appStop = PSIPHON_LINK_PREFIX _T("stop");
    const LPCTSTR appSaveSettings = PSIPHON_LINK_PREFIX _T("savesettings?");
    const size_t appSaveSettingsLen = _tcslen(appSaveSettings);
    const LPCTSTR appSendFeedback = PSIPHON_LINK_PREFIX _T("sendfeedback?");
    const size_t appSendFeedbackLen = _tcslen(appSendFeedback);
    const LPCTSTR appSetCookies = PSIPHON_LINK_PREFIX _T("setcookies?");
    const size_t appSetCookiesLen = _tcslen(appSetCookies);
    const LPCTSTR appBannerClick = PSIPHON_LINK_PREFIX _T("bannerclick");

    if (_tcscmp(url, appReady) == 0)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Ready requested"), __TFUNCTION__);
        g_htmlUiReady = true;
        PostMessage(g_hWnd, WM_PSIPHON_CREATED, 0, 0);
    }
    else if (_tcsncmp(url, appStringTable, appStringTableLen) == 0
        && _tcslen(url) > appStringTableLen)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: String table addition requested"), __TFUNCTION__);

        tstring urlDecoded = UrlDecode(url);
        if (urlDecoded.length() < appStringTableLen + 1)
        {
            return;
        }

        string stringJSON(TStringToNarrow(urlDecoded).c_str() + appStringTableLen);

        AddStringTableEntry(stringJSON);
    }
    else if (_tcscmp(url, appStart) == 0)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Start requested"), __TFUNCTION__);
        g_connectionManager.Start();
    }
    else if (_tcscmp(url, appStop) == 0)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Stop requested"), __TFUNCTION__);
        g_connectionManager.Stop(STOP_REASON_USER_DISCONNECT);
    }
    else if (_tcsncmp(url, appSaveSettings, appSaveSettingsLen) == 0
        && _tcslen(url) > appSaveSettingsLen)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Update settings requested"), __TFUNCTION__);
        
        tstring urlDecoded = UrlDecode(url);
        if (urlDecoded.length() < appSaveSettingsLen + 1)
        {
            return;
        }

        string stringJSON(TStringToNarrow(urlDecoded).c_str() + appSaveSettingsLen);
        bool settingsChanged = false;
        if (Settings::FromJson(stringJSON, settingsChanged) && settingsChanged)
        {
            // Refresh settings in UI
            Json::Value settingsJSON;
            Settings::ToJson(settingsJSON);
            Json::FastWriter jsonWriter;
            UI_RefreshSettings(jsonWriter.write(settingsJSON));

            if (g_connectionManager.GetState() == CONNECTION_MANAGER_STATE_CONNECTED
                || g_connectionManager.GetState() == CONNECTION_MANAGER_STATE_STARTING)
            {
                // Reconnect.
                my_print(NOT_SENSITIVE, false, _T("Settings change detected. Reconnecting."));
                g_connectionManager.Stop(STOP_REASON_USER_DISCONNECT);
                g_connectionManager.Start();
            }
        }
    }
    else if (_tcsncmp(url, appSendFeedback, appSendFeedbackLen) == 0
        && _tcslen(url) > appSendFeedbackLen)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Send feedback requested"), __TFUNCTION__);
        tstring urlDecoded = UrlDecode(url);
        if (urlDecoded.length() < appSendFeedbackLen + 1)
        {
            return;
        }

        string stringJSON(TStringToNarrow(urlDecoded).c_str() + appSendFeedbackLen);
        my_print(NOT_SENSITIVE, false, _T("Sending feedback..."));
        g_connectionManager.SendFeedback(NarrowToTString(stringJSON).c_str());
    }
    else if (_tcsncmp(url, appSetCookies, appSetCookiesLen) == 0
        && _tcslen(url) > appSetCookiesLen)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Set cookies requested"), __TFUNCTION__);
        tstring urlDecoded = UrlDecode(url);
        if (urlDecoded.length() < appSetCookiesLen + 1)
        {
            return;
        }

        string stringJSON(TStringToNarrow(urlDecoded).c_str() + appSetCookiesLen);
        Settings::SetCookies(stringJSON);
    }
    else if (_tcscmp(url, appBannerClick) == 0)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Banner clicked"), __TFUNCTION__);
        // If connected, open sponsor home pages, or info link if
        // no sponsor pages. If not connected, open info link.
        if (CONNECTION_MANAGER_STATE_CONNECTED == g_connectionManager.GetState())
        {
            g_connectionManager.OpenHomePages(INFO_LINK_URL, false);
        }
        else
        {
            OpenBrowser(INFO_LINK_URL);
        }
    }
    else {
        // Not one of our links. Open it in an external browser.
        OpenBrowser(url);
    }

    delete[] url;
}

//==== Exported functions ========================================================

void UI_SetStateStopped()
{
    UpdateSystrayConnectedState();

    Json::Value json;
    json["state"] = "stopped";
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}

void UI_SetStateStopping()
{
    UpdateSystrayConnectedState();

    Json::Value json;
    json["state"] = "stopping";
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}

void UI_SetStateStarting(const tstring& transportProtocolName)
{
    UpdateSystrayConnectedState();

    Json::Value json;
    json["state"] = "starting";
    json["transport"] = WStringToUTF8(transportProtocolName.c_str());
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}

void UI_SetStateConnected(const tstring& transportProtocolName, int socksPort, int httpPort)
{
    UpdateSystrayConnectedState();

    Json::Value json;
    json["state"] = "connected";
    json["transport"] = WStringToUTF8(transportProtocolName.c_str());
    json["socksPort"] = socksPort;
    json["socksPortAuto"] = Settings::LocalSocksProxyPort() == 0;
    json["httpPort"] = httpPort;
    json["httpPortAuto"] = Settings::LocalHttpProxyPort() == 0;
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}

void UI_Notice(const string& noticeJSON)
{
    HtmlUI_AddNotice(noticeJSON);
}

void UI_RefreshSettings(const string& settingsJSON)
{
    HtmlUI_RefreshSettings(settingsJSON);
}

//==== Win32 boilerplate ======================================================

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY _tWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPTSTR lpCmdLine,
    int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    LoadString(hInstance, IDS_APP_TITLE, g_szTitle, MAX_LOADSTRING);
    LoadString(hInstance, IDC_PSICLIENT, g_szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    /* Register mCtrl and its HTML control. */
    mc_StaticLibInitialize();
    mcHtml_Initialize();

    // Perform application initialization

    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable;
    hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_PSICLIENT));

    // If this set of calls gets any longer, we may want to do something generic.
    DoStartupSystemProxyWork();
    DoStartupDiagnosticCollection();

    // Main message loop

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            // Bit of a dirty hack to prevent the HTML control code from crashing
            // on exit.
            // WM_APP+2 is the message used for MC_HN_STATUSTEXT. Sometimes it
            // arrives after the control is destroyed and will cause an app
            // crash if we let it through.
            if (msg.message == (WM_APP + 2) && g_htmlUiFinished)
            {
                continue;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    SystrayIconCleanup();

    mcHtml_Terminate();
    mc_StaticLibTerminate();

    return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEX wcex = { 0 };

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PSICLIENT));
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = 0;
    wcex.lpszClassName = g_szWindowClass;
    wcex.hIconSm = NULL;

    return RegisterClassEx(&wcex);
}

//==== Main window functions ==================================================

#define WINDOW_X_START  780
#define WINDOW_Y_START  580
#define WINDOW_X_MIN    680
#define WINDOW_Y_MIN    580

static void SaveWindowPlacement()
{
    if (!g_hWnd)
    {
        return;
    }

    WINDOWPLACEMENT wp = { 0 };
    wp.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(g_hWnd, &wp))
    {
        return;
    }

    Json::Value json;
    json["showCmd"] = wp.showCmd;
    json["rcNormalPosition.top"] = wp.rcNormalPosition.top;
    json["rcNormalPosition.bottom"] = wp.rcNormalPosition.bottom;
    json["rcNormalPosition.left"] = wp.rcNormalPosition.left;
    json["rcNormalPosition.right"] = wp.rcNormalPosition.right;
    Json::FastWriter jsonWriter;
    Settings::SetWindowPlacement(jsonWriter.write(json));
}

static void RestoreWindowPlacement()
{
    if (!g_hWnd)
    {
        return;
    }

    string windowPlacementJson = Settings::GetWindowPlacement();
    if (windowPlacementJson.empty())
    {
        // No stored placement
        return;
    }

    WINDOWPLACEMENT wp = { 0 };
    wp.length = sizeof(WINDOWPLACEMENT);

    Json::Value json;
    Json::Reader reader;
    bool parsingSuccessful = reader.parse(windowPlacementJson, json);
    if (!parsingSuccessful)
    {
        my_print(NOT_SENSITIVE, false, _T("Failed to parse previous window placement"));
        return;
    }

    try
    {
        if (!json.isMember("showCmd") ||
            !json.isMember("rcNormalPosition.top") ||
            !json.isMember("rcNormalPosition.bottom") ||
            !json.isMember("rcNormalPosition.left") ||
            !json.isMember("rcNormalPosition.right"))
        {
            // The stored values are invalid
            return;
        }

        wp.showCmd = json.get("showCmd", SW_SHOWNORMAL).asUInt();
        // Don't allow restoring minimized
        if (wp.showCmd == SW_SHOWMINIMIZED)
        {
            wp.showCmd = SW_SHOWNORMAL;
        }

        wp.rcNormalPosition.top = (LONG)json.get("rcNormalPosition.top", 0).asLargestInt();
        wp.rcNormalPosition.bottom = (LONG)json.get("rcNormalPosition.bottom", WINDOW_Y_START).asLargestInt();
        wp.rcNormalPosition.left = (LONG)json.get("rcNormalPosition.left", 0).asLargestInt();
        wp.rcNormalPosition.right = (LONG)json.get("rcNormalPosition.right", WINDOW_X_START).asLargestInt();
    }
    catch (exception& e)
    {
        my_print(NOT_SENSITIVE, false, _T("%s:%d: JSON parse exception: %S"), __TFUNCTION__, __LINE__, e.what());
        return;
    }

    SetWindowPlacement(g_hWnd, &wp);
}

static LRESULT HandleNotify(HWND hWnd, NMHDR* hdr)
{
    if (hdr->idFrom == IDC_HTML_CTRL)
    {
        if (hdr->code == MC_HN_BEFORENAVIGATE)
        {
            MC_NMHTMLURL* nmHtmlUrl = (MC_NMHTMLURL*)hdr;
            // We should not interfere with the initial page load
            static bool s_firstNav = true;
            if (s_firstNav) {
                s_firstNav = false;
                return 0;
            }

            HtmlUI_BeforeNavigate(nmHtmlUrl);
            return -1; // Prevent navigation
        }
        else if (hdr->code == MC_HN_NEWWINDOW)
        {
            MC_NMHTMLURL* nmHtmlUrl = (MC_NMHTMLURL*)hdr;
            // Prevent new window from opening
            return 0;
        }
        else if (hdr->code == MC_HN_HTTPERROR)
        {
            MC_NMHTTPERROR* nmHttpError = (MC_NMHTTPERROR*)hdr;
            assert(false);
            // Prevent HTTP error from being shown.
            return 0;
        }
    }

    return 0;
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    // Don't allow multiple instances of this application to run
    if (g_singleInstanceObject.IsAnotherInstanceRunning())
    {
        HWND otherWindow = FindWindow(g_szWindowClass, g_szTitle);
        if (otherWindow)
        {
            // Un-minimize if necessary
            WINDOWPLACEMENT wp = { 0 };
            wp.length = sizeof(WINDOWPLACEMENT);
            if (GetWindowPlacement(otherWindow, &wp) &&
                wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_HIDE)
            {
                ShowWindow(otherWindow, SW_RESTORE);
            }

            ShowWindow(otherWindow, SW_SHOW);
            SetForegroundWindow(otherWindow);
        }
        return FALSE;
    }

    g_hInst = hInstance;

    g_hWnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        g_szWindowClass,
        g_szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_X_START, WINDOW_Y_START,
        NULL, NULL, hInstance, NULL);

    // Don't show the window until the content loads.

    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        OnCreate(hWnd);
        break;

    case WM_PSIPHON_CREATED:
    {
        // Display client version number
        my_print(NOT_SENSITIVE, false, (tstring(_T("Client Version: ")) + NarrowToTString(CLIENT_VERSION)).c_str());

        // Content is loaded, so show the window.
        RestoreWindowPlacement();
        ShowWindow(g_hWnd, SW_SHOW);

        // Start a connection
        if (!Settings::SkipAutoConnect())
        {
            g_connectionManager.Toggle();
        }
        break;
    }

    case WM_PSIPHON_HTMLUI_BEFORENAVIGATE:
        HtmlUI_BeforeNavigateHandler((LPCTSTR)wParam);
        break;
    case WM_PSIPHON_HTMLUI_SETSTATE:
        HtmlUI_SetStateHandler((LPCWSTR)wParam);
        break;
    case WM_PSIPHON_HTMLUI_ADDMESSAGE:
        HtmlUI_AddMessageHandler((LPCWSTR)wParam);
        break;
    case WM_PSIPHON_HTMLUI_ADDNOTICE:
        HtmlUI_AddNoticeHandler((LPCWSTR)wParam);
        break;
    case WM_PSIPHON_HTMLUI_REFRESHSETTINGS:
        HtmlUI_RefreshSettingsHandler((LPCWSTR)wParam);
        break;

    case WM_SIZE:
        if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED)
        {
            OnResize(hWnd, LOWORD(lParam), HIWORD(lParam));
        }
        break;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = WINDOW_X_MIN;
        mmi->ptMinTrackSize.y = WINDOW_Y_MIN;
        break;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE)
        {
            HandleMinimize();
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_SETFOCUS:
        SetFocus(g_hHtmlCtrl);
        break;

    case WM_NOTIFY:
        return HandleNotify(hWnd, (NMHDR*)lParam);

    case WM_PSIPHON_TRAY_ICON_NOTIFY:
        // Restore/foreground the app on any kind of click
        if (lParam == WM_LBUTTONUP || 
            lParam == WM_LBUTTONDBLCLK ||
            lParam == WM_RBUTTONUP ||
            lParam == WM_RBUTTONDBLCLK ||
            lParam == NIN_BALLOONUSERCLICK)
        {
            WINDOWPLACEMENT wp = { 0 };
            wp.length = sizeof(WINDOWPLACEMENT);
            if (GetWindowPlacement(g_hWnd, &wp) &&
                wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_HIDE)
            {
                ShowWindow(g_hWnd, SW_RESTORE);
            }

            ShowWindow(g_hWnd, SW_SHOW);
            SetForegroundWindow(g_hWnd);
        }
        break;

    case WM_PSIPHON_MY_PRINT:
    {
        int priority = (int)wParam;
        TCHAR* message = (TCHAR*)lParam;
        HtmlUI_AddMessage(priority, message);
        OutputDebugString(message);
        OutputDebugString(L"\n");
        free(message);
        break;
    }

    case WM_PSIPHON_FEEDBACK_SUCCESS:
        my_print(NOT_SENSITIVE, false, _T("Feedback sent. Thank you!"));
        break;

    case WM_PSIPHON_FEEDBACK_FAILED:
        my_print(NOT_SENSITIVE, false, _T("Failed to send feedback."));
        break;

    case WM_ENDSESSION:
        // Stop the tunnel -- particularly to ensure system proxy settings are reverted -- on OS shutdown
        // Note: due to the following bug, the system proxy settings revert may silently fail:
        // https://connect.microsoft.com/IE/feedback/details/838086/internet-explorer-10-11-wininet-api-drops-proxy-change-events-during-system-shutdown
    case WM_DESTROY:
        // Stop transport if running
        g_connectionManager.Stop(STOP_REASON_EXIT);
        g_htmlUiFinished = true;
        SaveWindowPlacement();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}
