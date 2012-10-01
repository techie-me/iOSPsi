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

#include "stdafx.h"
#include "shellapi.h"
#include "config.h"
#include "psiclient.h"
#include "connectionmanager.h"
#include "server_request.h"
#include "httpsrequest.h"
#include "webbrowser.h"
#include "embeddedvalues.h"
#include "usersettings.h"
#include "systemproxysettings.h"
#include "zlib.h"
#include <algorithm>
#include <sstream>
#include <Shlwapi.h>
#include "transport.h"
#include "transport_registry.h"
#include "transport_connection.h"
#include "server_entry_auth.h"
#include "stopsignal.h"


// Upgrade process posts a Quit message
extern HWND g_hWnd;


ConnectionManager::ConnectionManager(void) :
    m_state(CONNECTION_MANAGER_STATE_STOPPED),
    m_thread(0),
    m_upgradeThread(0),
    m_feedbackThread(0),
    m_startingTime(0),
    m_transport(0),
    m_upgradePending(false),
    m_startSplitTunnel(false),
    m_nextFetchRemoteServerListAttempt(0)
{
    m_mutex = CreateMutex(NULL, FALSE, 0);

    InitializeUserSettings();
}

ConnectionManager::~ConnectionManager(void)
{
    Stop(STOP_REASON_NONE);
    CloseHandle(m_mutex);
}

ServerList& ConnectionManager::GetServerList()
{
    return m_serverList;
}

void ConnectionManager::OpenHomePages(const TCHAR* defaultHomePage/*=0*/)
{
    AutoMUTEX lock(m_mutex);
    
    if (!UserSkipBrowser())
    {
        vector<tstring> urls = m_currentSessionInfo.GetHomepages();
        if (urls.size() == 0 && defaultHomePage)
        {
            urls.push_back(defaultHomePage);
        }
        OpenBrowser(urls);
    }
}

void ConnectionManager::Toggle(const tstring& transport, bool startSplitTunnel)
{
    // NOTE: no lock, to allow thread to access object

    if (m_state == CONNECTION_MANAGER_STATE_STOPPED)
    {
        Start(transport,startSplitTunnel);
    }
    else
    {
        Stop(STOP_REASON_USER_DISCONNECT);
    }
}

time_t ConnectionManager::GetStartingTime()
{
    // NOTE: no lock, to prevent blocking connection thread with UI polling
    // Starting Time is informational only, consistency with state isn't critical

    if (m_state != CONNECTION_MANAGER_STATE_STARTING)
    {
        return 0;
    }
    else
    {
        return time(0) - m_startingTime;
    }
}

void ConnectionManager::SetState(ConnectionManagerState newState)
{
    // NOTE: no lock, to prevent blocking connection thread with UI polling
    // Starting Time is informational only, consistency with state isn't critical

    if (newState == CONNECTION_MANAGER_STATE_STARTING)
    {
        m_startingTime = time(0);
    }
    else
    {
        m_startingTime = 0;
    }

    m_state = newState;
}

ConnectionManagerState ConnectionManager::GetState()
{
    return m_state;
}

void ConnectionManager::Stop(DWORD reason)
{
    my_print(true, _T("%s: enter"), __TFUNCTION__);

    // NOTE: no lock, to allow thread to access object

    // The assumption is that signalling stop will cause any current operations to
    // stop (such as making HTTPS requests, or establishing a connection), and
    // cause the connection to hang up if it is connected.
    // While a connection is active, there is a thread running waiting for the
    // connection to terminate.

    // This will signal (some) running tasks to terminate.
    GlobalStopSignal::Instance().SignalStop(reason);

    // Wait for thread to exit (otherwise can get access violation when app terminates)
    if (m_thread)
    {
        my_print(true, _T("%s: Waiting for thread to die"), __TFUNCTION__);
        WaitForSingleObject(m_thread, INFINITE);
        my_print(true, _T("%s: Thread died"), __TFUNCTION__);
        m_thread = 0;
    }

    if (m_upgradeThread)
    {
        my_print(true, _T("%s: Waiting for upgrade thread to die"), __TFUNCTION__);
        WaitForSingleObject(m_upgradeThread, INFINITE);
        my_print(true, _T("%s: Upgrade thread died"), __TFUNCTION__);
        m_upgradeThread = 0;
    }

    if (m_feedbackThread)
    {
        my_print(true, _T("%s: Waiting for feedback thread to die"), __TFUNCTION__);
        WaitForSingleObject(m_feedbackThread, INFINITE);
        my_print(true, _T("%s: Feedback thread died"), __TFUNCTION__);
        m_feedbackThread = 0;
    }

    delete m_transport;
    m_transport = 0;

    my_print(true, _T("%s: exit"), __TFUNCTION__);
}

void ConnectionManager::FetchRemoteServerList(void)
{
    AutoMUTEX lock(m_mutex);

    if (strlen(REMOTE_SERVER_LIST_ADDRESS) == 0)
    {
        return;
    }

    // After at least one failed connection attempt, and no more than once
    // per few hours (if successful), or not more than once per few minutes
    // (if unsuccessful), check for a new remote server list.
    if (m_nextFetchRemoteServerListAttempt != 0 &&
        m_nextFetchRemoteServerListAttempt > time(0))
    {
        return;
    }

    m_nextFetchRemoteServerListAttempt = time(0) + SECONDS_BETWEEN_UNSUCCESSFUL_REMOTE_SERVER_LIST_FETCH;

    string response;

    try
    {
        HTTPSRequest httpsRequest;
        // NOTE: Not using local proxy
        if (!httpsRequest.MakeRequest(
                NarrowToTString(REMOTE_SERVER_LIST_ADDRESS).c_str(),
                443,
                "",
                NarrowToTString(REMOTE_SERVER_LIST_REQUEST_PATH).c_str(),
                response,
                StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_EXIT),
                false) // don't use local proxy
            || response.length() <= 0)
        {
            my_print(false, _T("Fetch remote server list failed"));
            return;
        }
    }
    catch (StopSignal::StopException&)
    {
        // Application is exiting.
        return;
    }

    m_nextFetchRemoteServerListAttempt = time(0) + SECONDS_BETWEEN_SUCCESSFUL_REMOTE_SERVER_LIST_FETCH;

    string serverEntryList;
    if (!verifySignedServerList(response.c_str(), serverEntryList))
    {
        my_print(false, _T("Verify remote server list failed"));
        return;
    }

    vector<string> newServerEntryVector;
    istringstream serverEntryListStream(serverEntryList);
    string line;
    while (getline(serverEntryListStream, line))
    {
        if (!line.empty())
        {
            newServerEntryVector.push_back(line);
        }
    }
    m_serverList.AddEntriesToList(newServerEntryVector, 0);
}

void ConnectionManager::Start(const tstring& transport, bool startSplitTunnel)
{
    my_print(true, _T("%s: enter"), __TFUNCTION__);

    // Call Stop to cleanup in case thread failed on last Start attempt
    Stop(STOP_REASON_USER_DISCONNECT);

    AutoMUTEX lock(m_mutex);

    m_transport = TransportRegistry::New(transport);
    m_startSplitTunnel = startSplitTunnel;

    GlobalStopSignal::Instance().ClearStopSignal(STOP_REASON_USER_DISCONNECT | STOP_REASON_UNEXPECTED_DISCONNECT);

    if (m_state != CONNECTION_MANAGER_STATE_STOPPED || m_thread != 0)
    {
        my_print(false, _T("Invalid connection manager state in Start (%d)"), m_state);
        return;
    }

    SetState(CONNECTION_MANAGER_STATE_STARTING);

    if (!(m_thread = CreateThread(0, 0, ConnectionManagerStartThread, (void*)this, 0, 0)))
    {
        my_print(false, _T("Start: CreateThread failed (%d)"), GetLastError());

        SetState(CONNECTION_MANAGER_STATE_STOPPED);
    }

    my_print(true, _T("%s: exit"), __TFUNCTION__);
}

void ConnectionManager::StartSplitTunnel()
{
    AutoMUTEX lock(m_mutex);
    
    // Polipo is watching for changes to this file.
    // Note: there's some delay before the file change takes effect.
    WriteSplitTunnelRoutes(m_splitTunnelRoutes.c_str());
}

void ConnectionManager::StopSplitTunnel()
{
    AutoMUTEX lock(m_mutex);
    
    // See comment in StartSplitTunnel.
    DeleteSplitTunnelRoutes();
}

DWORD WINAPI ConnectionManager::ConnectionManagerStartThread(void* object)
{
    my_print(true, _T("%s: enter"), __TFUNCTION__);

    ConnectionManager* manager = (ConnectionManager*)object;

    // Seed built-in non-crypto PRNG used for shuffling (load balancing)
    unsigned int seed = (unsigned)time(NULL);
    srand(seed);

    //
    // Loop through server list, attempting to connect.
    //
    // When handshake and all connection types fail, the
    // server is marked as failed in the local server list and
    // the next server from the list is selected and retried.
    //
    // All operations may be interrupted by user cancel.
    //
    // NOTE: this function doesn't hold the ConnectionManager
    // object lock to allow for cancel etc.

    while (true) // Try servers loop
    {
        if (manager->m_upgradePending)
        {
            // An upgrade has been downloaded and paved.  Since there is no
            // currently connected tunnel, go ahead and restart the application
            // using the new version.
            // TODO: if ShellExecute fails, don't die?
            TCHAR filename[1000];
            if (GetModuleFileName(NULL, filename, 1000))
            {
                ShellExecute(0, NULL, filename, 0, 0, SW_SHOWNORMAL);
                PostMessage(g_hWnd, WM_QUIT, 0, 0);
                break;
            }
        }

        my_print(true, _T("%s: enter server loop"), __TFUNCTION__);

        try
        {
            // Get the next server to try

            tstring handshakeRequestPath;

            manager->LoadNextServer(handshakeRequestPath);

            // Note that the SessionInfo will only be partly filled in at this point.
            SessionInfo sessionInfo;
            manager->CopyCurrentSessionInfo(sessionInfo);

            //
            // Set up the transport connection
            //

            my_print(true, _T("%s: doing transportConnection for %s"), __TFUNCTION__, manager->m_transport->GetTransportDisplayName().c_str());

            // Note that the TransportConnection will do any necessary cleanup.
            TransportConnection transportConnection;

            // May throw TryNextServer
            transportConnection.Connect(
                StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_ALL),
                manager->m_transport,
                manager,
                sessionInfo,
                handshakeRequestPath.c_str(),
                manager->GetSplitTunnelingFilePath());

            //
            // The transport connection did a handshake, so its sessionInfo is 
            // fuller than ours. Update ours and then update the server entries.
            //

            sessionInfo = transportConnection.GetUpdatedSessionInfo();
            manager->UpdateCurrentSessionInfo(sessionInfo);

            //
            // If handshake notified of new version, start the upgrade in a (background) thread
            //

            if (manager->RequireUpgrade())
            {
                if (!manager->m_upgradeThread ||
                    WAIT_OBJECT_0 == WaitForSingleObject(manager->m_upgradeThread, 0))
                {
                    if (!(manager->m_upgradeThread = CreateThread(0, 0, ConnectionManagerUpgradeThread, manager, 0, 0)))
                    {
                        my_print(false, _T("Upgrade: CreateThread failed (%d)"), GetLastError());
                    }
                }
            }

            // Before doing post-connect work, make sure there's no stop signal.
            // Throws if there's a signal set.
            GlobalStopSignal::Instance().CheckSignal(STOP_REASON_ALL, true);

            //
            // Do post-connect work, like opening home pages.
            //

            my_print(true, _T("%s: transport succeeded; DoPostConnect"), __TFUNCTION__);
            manager->DoPostConnect(sessionInfo);

            //
            // Wait for transportConnection to stop (or fail)
            //

            my_print(true, _T("%s: entering transportConnection wait"), __TFUNCTION__);
            transportConnection.WaitForDisconnect();

            //
            // Disconnected
            //

            manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);

            my_print(true, _T("%s: breaking"), __TFUNCTION__);
            break;
        }
        catch (IWorkerThread::Error& error)
        {
            // Unrecoverable error. Cleanup and exit.
            my_print(true, _T("%s: caught ITransport::Error: %s"), __TFUNCTION__, error.GetMessage().c_str());
            manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);
            break;
        }
        catch (IWorkerThread::Abort&)
        {
            // User requested cancel. Cleanup and exit.
            my_print(true, _T("%s: caught IWorkerThread::Abort"), __TFUNCTION__);
            manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);
            break;
        }
        // Catch the StopException base class
        catch (StopSignal::StopException&)
        {
            // User requested cancel or transport died, etc. Cleanup and exit.
            my_print(true, _T("%s: caught StopSignal::StopException"), __TFUNCTION__);
            manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);
            break;
        }
        catch (ConnectionManager::Abort&)
        {
            my_print(true, _T("%s: caught ConnectionManager::Abort"), __TFUNCTION__);
            manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);
            break;
        }
        catch (TransportConnection::TryNextServer&)
        {
            // Failed to connect to the server. Try the next one.
            my_print(true, _T("%s: caught TryNextServer"), __TFUNCTION__);
            manager->MarkCurrentServerFailed();

            // Give users some feedback. Before, when the handshake failed
            // all we displayed was "WinHttpCallbackFailed (200000)" and kept
            // the arrow animation spinning. A user-authored FAQ mentioned
            // this error in particular and recommended waiting. So here's
            // a lightly more encouraging message.
            my_print(false, _T("Trying next server..."));

            // Continue while loop to try next server

            manager->FetchRemoteServerList();

            // Wait between 1 and 2 seconds before retrying. This is a quick
            // fix to deal with the following problem: when a client can
            // make an HTTPS connection but not a VPN connection, it ends
            // up spamming "handshake" requests, resulting in PSK race conditions
            // with other clients that are trying to connect. This is starving
            // clients that are able to establish the VPN connection.
            // TODO: a more optimal solution would only wait when re-trying
            // a server where this condition (HTTPS ok, VPN failed) previously
            // occurred.
            // UPDATE: even with SSH as a fail over, we're leaving this delay
            // in for now as clients blocked on both protocols would otherwise
            // still spam handshakes. The delay is *after* SSH fail over so as
            // not to delay that attempt (on the same server).
            Sleep(1000 + rand()%1000);    
        }
    }

    my_print(true, _T("%s: exiting thread"), __TFUNCTION__);
    return 0;
}

#if defined TESTING && defined _DEBUG
void WriteSessionInfo(const SessionInfo& sessionInfo, ITransport* transport)
{
    ofstream out("..\\..\\Test\\test-conn-info.json");
    if (!out) 
    { 
        my_print(true, _T("%s: cannot open session info file"), __TFUNCTION__);
        return; 
    }

    out << "{\n" 
        << "\"server_ip\": \"" << sessionInfo.GetServerAddress() << "\",\n"
        << "\"server_web_port\": " << sessionInfo.GetWebPort() << ",\n"
        << "\"client_session_id\": \"" << sessionInfo.GetClientSessionID() << "\",\n"
        << "\"propagation_channel_id\": \"" << PROPAGATION_CHANNEL_ID << "\",\n"
        << "\"sponsor_id\": \"" << SPONSOR_ID << "\",\n"
        << "\"client_version\": \"" << CLIENT_VERSION << "\",\n"
        << "\"server_secret\": \"" << sessionInfo.GetWebServerSecret() << "\",\n"
        << "\"relay_protocol\": \"" << TStringToNarrow(transport->GetTransportProtocolName()) << "\",\n"
        << "\"session_id\": \"" << TStringToNarrow(transport->GetSessionID(sessionInfo)) << "\",\n"
        << "\"last_connected\": \"" << "None" << "\",\n"
        << "\"ssh_port\": " << sessionInfo.GetSSHPort() << ",\n"
        << "\"ssh_username\": \"" << sessionInfo.GetSSHUsername() << "\",\n"
        << "\"ssh_password\": \"" << sessionInfo.GetSSHPassword() << "\",\n"
        << "\"ssh_hostkey\": \"" << sessionInfo.GetSSHHostKey() << "\",\n"
        << "\"ssh_obfsport\": \"" << sessionInfo.GetSSHObfuscatedPort() << "\",\n"
        << "\"ssh_obfskey\": \"" << sessionInfo.GetSSHObfuscatedKey() << "\",\n"
        << "\"http_proxy_port\": " << DEFAULT_LOCAL_HTTP_PROXY_PORT << "\n"
        << "}";

    out.close();
}
#endif //TESTING

void ConnectionManager::DoPostConnect(const SessionInfo& sessionInfo)
{
    // Called from connection thread
    // NOTE: no lock while waiting for network events

#if defined TESTING && defined _DEBUG
    WriteSessionInfo(sessionInfo, m_transport);
#endif

    SetState(CONNECTION_MANAGER_STATE_CONNECTED);

    //
    // "Connected" HTTPS request for server stats and split tunnel routing info.
    // It's not critical if this request fails so failure is ignored.
    //
    
    tstring connectedRequestPath = GetConnectRequestPath(m_transport);
        
    DWORD start = GetTickCount();
    string response;
    ServerRequest serverRequest;
    if (serverRequest.MakeRequest(
                        false, // don't allow adhoc
                        m_transport,
                        sessionInfo,
                        connectedRequestPath.c_str(),
                        response,
                        StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_ALL)))
    {
        // Record the request time.
        (void)WriteRegistryStringValue(
                LOCAL_SETTINGS_REGISTRY_VALUE_LAST_CONNECTED, 
                TStringToNarrow(GetISO8601DatetimeString()));

        // Speed feedback
        // Note: the /connected request *is* tunneled

        DWORD now = GetTickCount();
        if (now >= start) // GetTickCount can wrap
        {
            string speedResponse;
            ServerRequest serverRequest;
            (void)serverRequest.MakeRequest(
                            false, // don't allow adhoc
                            m_transport,
                            sessionInfo,
                            GetSpeedRequestPath(
                                m_transport->GetTransportProtocolName(),
                                _T("connected"),
                                _T(""),
                                now-start,
                                response.length()).c_str(),
                            speedResponse,
                            StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_ALL));
        }

        // Process split tunnel response
        ProcessSplitTunnelResponse(response);

        // Process flag to start split tunnel after initial connection
        if (m_startSplitTunnel)
        {
            StartSplitTunnel();
        }
    }

    //
    // Open home pages in browser
    //
    
    OpenHomePages();

    // Perform non-tunneled speed test when requested
    // Note that in VPN mode, the WinHttp request is implicitly tunneled.

    tstring speedTestServerAddress, speedTestRequestPath;
    int speedTestServerPort = 0;
    GetSpeedTestURL(speedTestServerAddress, speedTestServerPort, speedTestRequestPath);
    // HTTPSRequest is always https
    tstringstream speedTestURL;
    speedTestURL << _T("https://") << speedTestServerAddress << _T(":") << speedTestServerPort << speedTestRequestPath;

    if (speedTestServerAddress.length() > 0)
    {
        DWORD start = GetTickCount();
        string response;
        HTTPSRequest httpsRequest;
        bool success = false;
        if (httpsRequest.MakeRequest(
                            speedTestServerAddress.c_str(),
                            speedTestServerPort,
                            "",
                            speedTestRequestPath.c_str(),
                            response,
                            // Because it's not tunneled, in theory this doesn't 
                            // need to be STOP_REASON_ALL -- it could instead be 
                            // _EXIT. But we spawn a new speed test on each 
                            // connection, so we'd better clean this up each time
                            // the connection comes down (before the next comes up).
                            StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_ALL),
                            false)) // don't proxy
        {
            success = true;
        }
        DWORD now = GetTickCount();
        if (now >= start) // GetTickCount can wrap
        {
            string speedResponse;
            ServerRequest serverRequest;
            serverRequest.MakeRequest(
                            false, // don't allow adhoc
                            m_transport,
                            sessionInfo,
                            GetSpeedRequestPath(
                                m_transport->GetTransportProtocolName(),
                                success ? _T("speed_test") : _T("speed_test_failure"),
                                speedTestURL.str().c_str(),
                                now-start,
                                response.length()).c_str(),
                            speedResponse,
                            StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_ALL));
        }
    }
}

bool ConnectionManager::SendStatusMessage(
                            bool final,
                            const map<string, int>& pageViewEntries,
                            const map<string, int>& httpsRequestEntries,
                            unsigned long long bytesTransferred)
{
    // NOTE: no lock while waiting for network events

    // Make a copy of SessionInfo for threadsafety.
    SessionInfo sessionInfo;
    {
        AutoMUTEX lock(m_mutex);
        sessionInfo = m_currentSessionInfo;
    }

    // Format stats data for consumption by the server. 

    Json::Value stats;
    stats["bytes_transferred"] = bytesTransferred;
    my_print(true, _T("BYTES: %llu"), bytesTransferred);

    map<string, int>::const_iterator pos = pageViewEntries.begin();
    Json::Value page_views(Json::arrayValue);
    for (; pos != pageViewEntries.end(); pos++)
    {
        Json::Value entry;
        entry["page"] = pos->first;
        entry["count"] = pos->second;
        page_views.append(entry);
        my_print(true, _T("PAGEVIEW: %d: %S"), pos->second, pos->first.c_str());
    }
    stats["page_views"] = page_views;

    pos = httpsRequestEntries.begin();
    Json::Value https_requests(Json::arrayValue);
    for (; pos != httpsRequestEntries.end(); pos++)
    {
        Json::Value entry;
        entry["domain"] = pos->first;
        entry["count"] = pos->second;
        https_requests.append(entry);
        my_print(true, _T("HTTPS REQUEST: %d: %S"), pos->second, pos->first.c_str());
    }
    stats["https_requests"] = https_requests;

    ostringstream additionalData; 
    Json::FastWriter jsonWriter;
    additionalData << jsonWriter.write(stats); 
    string additionalDataString = additionalData.str();

    tstring requestPath = GetStatusRequestPath(m_transport, !final);
    if (requestPath.length() <= 0)
    {
        // Can't send the status
        return false;
    }

    string response;
    ServerRequest serverRequest;

    // When disconnected, ignore the user cancel flag in the HTTP request
    // wait loop.
    // TODO: the user may be left waiting too long after cancelling; add
    // a shorter timeout in this case
    DWORD stopReason = final ? STOP_REASON_NONE : STOP_REASON_ALL;

    bool success = serverRequest.MakeRequest(
                                    final, // allow adhoc if this is the final stats request
                                    m_transport,
                                    sessionInfo,
                                    requestPath.c_str(),
                                    response,
                                    StopInfo(&GlobalStopSignal::Instance(), stopReason),
                                    L"Content-Type: application/json",
                                    (LPVOID)additionalDataString.c_str(),
                                    additionalDataString.length());
    
    return success;
}

tstring ConnectionManager::GetSpeedRequestPath(const tstring& relayProtocol, const tstring& operation, const tstring& info, DWORD milliseconds, DWORD size)
{
    AutoMUTEX lock(m_mutex);

    std::stringstream strMilliseconds;
    strMilliseconds << milliseconds;

    std::stringstream strSize;
    strSize << size;

    return tstring(HTTP_SPEED_REQUEST_PATH) + 
           _T("?client_session_id=") + NarrowToTString(m_currentSessionInfo.GetClientSessionID()) +
           _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
           _T("&relay_protocol=") + relayProtocol +
           _T("&operation=") + operation +
           _T("&info=") + info +
           _T("&milliseconds=") + NarrowToTString(strMilliseconds.str()) +
           _T("&size=") + NarrowToTString(strSize.str());
}

void ConnectionManager::GetSpeedTestURL(tstring& serverAddress, int& serverPort, tstring& requestPath)
{
    AutoMUTEX lock(m_mutex);

    serverAddress = NarrowToTString(m_currentSessionInfo.GetSpeedTestServerAddress());
    serverPort = m_currentSessionInfo.GetSpeedTestServerPort();
    requestPath = NarrowToTString(m_currentSessionInfo.GetSpeedTestRequestPath());
}

tstring ConnectionManager::GetFailedRequestPath(ITransport* transport)
{
    AutoMUTEX lock(m_mutex);

    return tstring(HTTP_FAILED_REQUEST_PATH) + 
           _T("?client_session_id=") + NarrowToTString(m_currentSessionInfo.GetClientSessionID()) +
           _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
           _T("&relay_protocol=") +  transport->GetTransportProtocolName() + 
           _T("&error_code=") + transport->GetLastTransportError();
}

tstring ConnectionManager::GetConnectRequestPath(ITransport* transport)
{
    AutoMUTEX lock(m_mutex);

    // Get info about the previous connected event
    string lastConnected;
    // Don't check the return value -- use the default empty string if not found.
    (void)ReadRegistryStringValue(LOCAL_SETTINGS_REGISTRY_VALUE_LAST_CONNECTED, lastConnected);
    if (lastConnected.length() == 0) lastConnected = "None";

    return tstring(HTTP_CONNECTED_REQUEST_PATH) + 
           _T("?client_session_id=") + NarrowToTString(m_currentSessionInfo.GetClientSessionID()) +
           _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
           _T("&relay_protocol=") + transport->GetTransportProtocolName() + 
           _T("&session_id=") + transport->GetSessionID(m_currentSessionInfo) +
           _T("&last_connected=") + NarrowToTString(lastConnected);
}

tstring ConnectionManager::GetStatusRequestPath(ITransport* transport, bool connected)
{
    AutoMUTEX lock(m_mutex);

    tstring sessionID = transport->GetSessionID(m_currentSessionInfo);

    // If there's no session ID, we can't send the status.
    if (sessionID.length() <= 0)
    {
        my_print(true, _T("%s: no session ID; not sending status"), __TFUNCTION__);
        return _T("");
    }

    // TODO: get error code from SSH client?

    return tstring(HTTP_STATUS_REQUEST_PATH) + 
           _T("?client_session_id=") + NarrowToTString(m_currentSessionInfo.GetClientSessionID()) +
           _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
           _T("&relay_protocol=") +  transport->GetTransportProtocolName() + 
           _T("&session_id=") + sessionID + 
           _T("&connected=") + (connected ? _T("1") : _T("0"));
}

void ConnectionManager::GetUpgradeRequestInfo(SessionInfo& sessionInfo, tstring& requestPath)
{
    AutoMUTEX lock(m_mutex);

    sessionInfo = m_currentSessionInfo;
    requestPath = tstring(HTTP_DOWNLOAD_REQUEST_PATH) + 
                    _T("?client_session_id=") + NarrowToTString(m_currentSessionInfo.GetClientSessionID()) +
                    _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
                    _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
                    _T("&client_version=") + NarrowToTString(m_currentSessionInfo.GetUpgradeVersion()) +
                    _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret());
}

tstring ConnectionManager::GetFeedbackRequestPath(ITransport* transport)
{
    AutoMUTEX lock(m_mutex);

    return tstring(HTTP_FEEDBACK_REQUEST_PATH) + 
           _T("?client_session_id=") + NarrowToTString(m_currentSessionInfo.GetClientSessionID()) +
           _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
           _T("&relay_protocol=") +  (transport ? transport->GetTransportProtocolName() : _T("")) + 
           _T("&session_id=") + (transport ? transport->GetSessionID(m_currentSessionInfo) : _T("")) + 
           _T("&connected=") + ((GetState() == CONNECTION_MANAGER_STATE_CONNECTED) ? _T("1") : _T("0"));
}

void ConnectionManager::MarkCurrentServerFailed(void)
{
    AutoMUTEX lock(m_mutex);
    
    m_serverList.MarkCurrentServerFailed();
}

// ==== General Session Functions =============================================

// Note that the SessionInfo structure will only be partly filled in by this function.
void ConnectionManager::LoadNextServer(tstring& handshakeRequestPath)
{
    // Select next server to try to connect to

    AutoMUTEX lock(m_mutex);

    ServerEntry serverEntry;
    
    try
    {
        // Try the next server in our list.
        serverEntry = m_serverList.GetNextServer();
    }
    catch (std::exception &ex)
    {
        my_print(false, string("LoadNextServer caught exception: ") + ex.what());
        throw Abort();
    }

    // Ensure split tunnel routes are reset before new session
    m_splitTunnelRoutes = "";
    StopSplitTunnel();

    // Current session holds server entry info and will also be loaded
    // with homepage and other info.
    m_currentSessionInfo.Set(serverEntry);

    // Generate a new client session ID to be included with all subsequent web requests
    m_currentSessionInfo.GenerateClientSessionID();

    // Output values used in next TryNextServer step

    handshakeRequestPath = tstring(HTTP_HANDSHAKE_REQUEST_PATH) + 
                           _T("?client_session_id=") + NarrowToTString(m_currentSessionInfo.GetClientSessionID()) +
                           _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
                           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
                           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
                           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
                           _T("&relay_protocol=") + m_transport->GetTransportProtocolName();

    // Include a list of known server IP addresses in the request query string as required by /handshake
    ServerEntries serverEntries =  m_serverList.GetList();
    for (ServerEntryIterator ii = serverEntries.begin(); ii != serverEntries.end(); ++ii)
    {
        handshakeRequestPath += _T("&known_server=");
        handshakeRequestPath += NarrowToTString(ii->serverAddress);
    }
}

bool ConnectionManager::RequireUpgrade(void)
{
    AutoMUTEX lock(m_mutex);

    return !m_upgradePending && m_currentSessionInfo.GetUpgradeVersion().size() > 0;
}

DWORD WINAPI ConnectionManager::ConnectionManagerUpgradeThread(void* object)
{
    my_print(true, _T("%s: enter"), __TFUNCTION__);

    my_print(false, _T("Downloading new version..."));

    ConnectionManager* manager = (ConnectionManager*)object;

    try
    {
        SessionInfo sessionInfo;
        tstring downloadRequestPath;
        string downloadResponse;
        // Note that this is getting the current session info, which is set
        // by LoadNextServer.  So it's unlikely but possible that we may be
        // loading the next server after the first one that notified us of an
        // upgrade failed to connect.  This still should not be a problem, since
        // all servers should have the same upgrades available.
        manager->GetUpgradeRequestInfo(sessionInfo, downloadRequestPath);

        // Download new binary
        DWORD start = GetTickCount();
        ServerRequest serverRequest;
        if (!serverRequest.MakeRequest(
                    false, // don't allow adhoc
                    manager->m_transport,
                    sessionInfo,
                    downloadRequestPath.c_str(),
                    downloadResponse,
                    StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_ALL)))
        {
            // If the download failed, we simply do nothing.
            // Rationale:
            // - The server is (and hopefully will remain) backwards compatible.
            // - The failure is likely a configuration one, as the handshake worked.
            // - A configuration failure could be common across all servers, so the
            //   client will never connect.
            // - Fail-over exposes new server IPs to hostile networks, so we don't
            //   like doing it in the case where we know the handshake already succeeded.
        }
        else
        {
            my_print(false, _T("Download complete"));

            // Speed feedback
            DWORD now = GetTickCount();
            if (now >= start) // GetTickCount can wrap
            {
                string speedResponse;
                (void)serverRequest.MakeRequest( // Ignore failure
                                false, // don't allow adhoc
                                manager->m_transport,
                                sessionInfo,
                                manager->GetSpeedRequestPath(
                                    _T(""),
                                    _T("download"),
                                    _T(""),
                                    now-start,
                                    downloadResponse.length()).c_str(),
                                speedResponse,
                                StopInfo(&GlobalStopSignal::Instance(), STOP_REASON_ALL));
            }

            // Perform upgrade.
        
            manager->PaveUpgrade(downloadResponse);
        }
    }
    catch (StopSignal::StopException&)
    {
        // do nothing, just exit
    }

    my_print(true, _T("%s: exiting thread"), __TFUNCTION__);
    return 0;
}

void ConnectionManager::PaveUpgrade(const string& download)
{
    AutoMUTEX lock(m_mutex);

    // Find current process binary path

    TCHAR filename[1000];
    if (!GetModuleFileName(NULL, filename, 1000))
    {
        // Abort upgrade
        return;
    }

    // Rename current binary to archive name

    tstring archive_filename(filename);
    archive_filename += _T(".orig");

    bool bArchiveCreated = false;

    try
    {
        // We can't delete/modify the binary for a running Windows process,
        // so instead we move the running binary to an archive filename and
        // write the new version to the original filename.

        if (!DeleteFile(archive_filename.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            throw std::exception("Upgrade - DeleteFile failed");
        }

        if (!MoveFile(filename, archive_filename.c_str()))
        {
            throw std::exception("Upgrade - MoveFile failed");
        }

        bArchiveCreated = true;

        // Write new version to current binary file name

        AutoHANDLE file = CreateFile(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (file == INVALID_HANDLE_VALUE)
        {
            throw std::exception("Upgrade - CreateFile failed");
        }

        DWORD written;

        if (!WriteFile(file, download.c_str(), download.length(), &written, NULL) || written != download.length())
        {
            throw std::exception("Upgrade - WriteFile failed");
        }

        if (!FlushFileBuffers(file))
        {
            throw std::exception("Upgrade - FlushFileBuffers failed");
        }
    }
    catch (std::exception& ex)
    {
        std::stringstream s;
        s << ex.what() << " (" << GetLastError() << ")";
        my_print(false, s.str().c_str());
        
        // Try to restore the original version
        if (bArchiveCreated)
        {
            CopyFile(archive_filename.c_str(), filename, FALSE);
        }

        // Abort upgrade
        return;
    }

    m_upgradePending = true;
}

void ConnectionManager::ProcessSplitTunnelResponse(const string& compressedRoutes)
{
    AutoMUTEX lock(m_mutex);

    // Decompress split tunnel route info
    // Defaults to blank route list on any error --> no split tunneling

    m_splitTunnelRoutes = "";

    if (compressedRoutes.length() == 0)
    {
        return;
    }

    const int CHUNK_SIZE = 1024;
    const int SANITY_CHECK_SIZE = 10*1024*1024;
    int ret;
    z_stream stream;
    char out[CHUNK_SIZE+1];

    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = compressedRoutes.length();
    stream.next_in = (unsigned char*)compressedRoutes.c_str();

    if (Z_OK != inflateInit(&stream))
    {
        return;
    }

    do
    {
        stream.avail_out = CHUNK_SIZE;
        stream.next_out = (unsigned char*)out;
        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END)
        {
            my_print(true, _T("ProcessSplitTunnelResponse failed (%d)"), ret);
            m_splitTunnelRoutes = "";
            break;
        }

        out[CHUNK_SIZE - stream.avail_out] = '\0';

        m_splitTunnelRoutes += out;

        if (m_splitTunnelRoutes.length() > SANITY_CHECK_SIZE)
        {
            my_print(true, _T("ProcessSplitTunnelResponse overflow"));
            m_splitTunnelRoutes = "";
            break;
        }

    } while (ret != Z_STREAM_END);

    inflateEnd(&stream);
}

tstring ConnectionManager::GetSplitTunnelingFilePath()
{
    TCHAR filePath[MAX_PATH];
    TCHAR tempPath[MAX_PATH];
    // http://msdn.microsoft.com/en-us/library/aa364991%28v=vs.85%29.aspx notes
    // tempPath can contain no more than MAX_PATH-14 characters
    int ret = GetTempPath(MAX_PATH, tempPath);
    if (ret > MAX_PATH-14 || ret == 0)
    {
        return _T("");
    }

    if(NULL != PathCombine(filePath, tempPath, SPLIT_TUNNELING_FILE_NAME))
    {
        return tstring(filePath);
    }
    return _T("");
}

bool ConnectionManager::WriteSplitTunnelRoutes(const char* routes)
{
    AutoMUTEX lock(m_mutex);

    tstring filePath = GetSplitTunnelingFilePath();
    if (filePath.length() == 0)
    {
        my_print(false, _T("WriteSplitTunnelRoutes - GetSplitTunnelingFilePath failed (%d)"), GetLastError());
        return false;
    }

    AutoHANDLE file = CreateFile(filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        my_print(false, _T("WriteSplitTunnelRoutes - CreateFile failed (%d)"), GetLastError());
        return false;
    }

    DWORD length = strlen(routes);
    DWORD written;
    if (!WriteFile(
            file,
            (unsigned char*)routes,
            length,
            &written,
            NULL)
          || written != length)
    {
        my_print(false, _T("WriteSplitTunnelRoutes - WriteFile failed (%d)"), GetLastError());
        return false;
    }

    return true;
}

bool ConnectionManager::DeleteSplitTunnelRoutes()
{
    AutoMUTEX lock(m_mutex);

    tstring filePath = GetSplitTunnelingFilePath();
    if (filePath.length() == 0)
    {
        my_print(false, _T("DeleteSplitTunnelRoutes - GetSplitTunnelingFilePath failed (%d)"), GetLastError());
        return false;
    }

    if (!DeleteFile(filePath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        my_print(false, _T("DeleteSplitTunnelRoutes - DeleteFile failed (%d)"), GetLastError());
        return false;
    }

    return true;
}

// Makes a thread-safe copy of m_currentSessionInfo
void ConnectionManager::CopyCurrentSessionInfo(SessionInfo& sessionInfo)
{
    AutoMUTEX lock(m_mutex);
    sessionInfo = m_currentSessionInfo;
}

// Makes a thread-safe copy of m_currentSessionInfo
void ConnectionManager::UpdateCurrentSessionInfo(const SessionInfo& sessionInfo)
{
    AutoMUTEX lock(m_mutex);
    m_currentSessionInfo = sessionInfo;

    try
    {
        m_serverList.AddEntriesToList(
            m_currentSessionInfo.GetDiscoveredServerEntries(), 
            &m_currentSessionInfo.GetServerEntry());
    }
    catch (std::exception &ex)
    {
        my_print(false, string("HandleHandshakeResponse caught exception: ") + ex.what());
        // This isn't fatal.  The transport connection can still be established.
    }
}

struct FeedbackThreadData
{
    ConnectionManager* connectionManager;
    wstring feedback;
} g_feedbackThreadData;

void ConnectionManager::SendFeedback(LPCWSTR feedback)
{
    g_feedbackThreadData.connectionManager = this;
    g_feedbackThreadData.feedback = feedback;

    if (!m_feedbackThread ||
        WAIT_OBJECT_0 == WaitForSingleObject(m_feedbackThread, 0))
    {
        if (!(m_upgradeThread = CreateThread(0, 0, ConnectionManager::ConnectionManagerFeedbackThread, (void*)&g_feedbackThreadData, 0, 0)))
        {
            my_print(false, _T("%s: CreateThread failed (%d)"), __TFUNCTION__, GetLastError());
            PostMessage(g_hWnd, WM_PSIPHON_FEEDBACK_FAILED, 0, 0);
            return;
        }
    }
}

DWORD WINAPI ConnectionManager::ConnectionManagerFeedbackThread(void* object)
{
    my_print(true, _T("%s: enter"), __TFUNCTION__);

    FeedbackThreadData* data = (FeedbackThreadData*)object;

    if (data->connectionManager->DoSendFeedback(data->feedback.c_str()))
    {
        PostMessage(g_hWnd, WM_PSIPHON_FEEDBACK_SUCCESS, 0, 0);
    }
    else
    {
        PostMessage(g_hWnd, WM_PSIPHON_FEEDBACK_FAILED, 0, 0);
    }

    my_print(true, _T("%s: exit"), __TFUNCTION__);
    return 0;
}

bool ConnectionManager::DoSendFeedback(LPCWSTR feedback)
{
    // NOTE: no lock while waiting for network events

    // Make a copy of SessionInfo for threadsafety.
    SessionInfo sessionInfo;
    {
        AutoMUTEX lock(m_mutex);
        sessionInfo = m_currentSessionInfo;
    }

    string narrowFeedback;
    if (feedback)
    {
        narrowFeedback = WStringToNarrow(feedback);
    }

    tstring requestPath = GetFeedbackRequestPath(m_transport);
    string response;
    HTTPSRequest httpsRequest;

    // When disconnected, ignore the user cancel flag in the HTTP request
    // wait loop.
    // TODO: the user may be left waiting too long after cancelling; add
    // a shorter timeout in this case
    DWORD stopReason = (GetState() == CONNECTION_MANAGER_STATE_CONNECTED ? STOP_REASON_ALL : STOP_REASON_NONE);

    // Use the system proxy unless the currently connected transport forbids it.
    bool useProxy = true;
    if (m_transport 
        && m_transport->IsConnected() 
        && !m_transport->IsServerRequestTunnelled())
    {
        useProxy = false;
    }

    bool success = httpsRequest.MakeRequest(
                                    NarrowToTString(sessionInfo.GetServerAddress()).c_str(),
                                    sessionInfo.GetWebPort() ,
                                    sessionInfo.GetWebServerCertificate(),
                                    requestPath.c_str(),
                                    response,
                                    StopInfo(&GlobalStopSignal::Instance(), stopReason),
                                    useProxy,
                                    L"Content-Type: application/json",
                                    (LPVOID)narrowFeedback.c_str(),
                                    narrowFeedback.length());
    
    return success;
}
