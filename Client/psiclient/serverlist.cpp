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
#include "psiclient.h"
#include "serverlist.h"
#include "embeddedvalues.h"
#include "config.h"
#include "utilities.h"
#include <algorithm>
#include <sstream>


ServerList::ServerList()
{
    m_mutex = CreateMutex(NULL, FALSE, 0);
}

ServerList::~ServerList()
{
    CloseHandle(m_mutex);
}

void ServerList::AddEntriesToList(const vector<string>& newServerEntryList)
{
    AutoMUTEX lock(m_mutex, __TFUNCTION__);

    if (newServerEntryList.size() < 1)
    {
        return;
    }

    ServerEntries oldServerEntryList = GetList();
    for (vector<string>::const_iterator newServerEntryString = newServerEntryList.begin();
         newServerEntryString != newServerEntryList.end(); ++newServerEntryString)
    {
        ServerEntry newServerEntry = ParseServerEntry(*newServerEntryString);

        // Check if we already know about this server
        bool alreadyKnown = false;
        for (ServerEntries::iterator oldServerEntry = oldServerEntryList.begin();
             oldServerEntry != oldServerEntryList.end(); ++oldServerEntry)
        {
            if (newServerEntry.serverAddress == oldServerEntry->serverAddress)
            {
                alreadyKnown = true;
                // NOTE: We always update the values for known servers, because we trust the
                //       discovery mechanisms
                oldServerEntry->Copy(newServerEntry);
                break;
            }
        }

        if (!alreadyKnown)
        {
            // Insert the new entry as the second entry, so that the first entry can continue
            // to be used if it is reachable
            oldServerEntryList.insert(oldServerEntryList.begin() + 1, newServerEntry);
        }
    }

    WriteListToSystem(oldServerEntryList);
}

void ServerList::MarkCurrentServerFailed()
{
    AutoMUTEX lock(m_mutex, __TFUNCTION__);

    ServerEntries serverEntryList = GetList();
    if (serverEntryList.size() > 1)
    {
        // Move the first server to the end of the list
        serverEntryList.push_back(serverEntryList[0]);
        serverEntryList.erase(serverEntryList.begin());
        WriteListToSystem(serverEntryList);
    }
}

ServerEntry ServerList::GetNextServer()
{
    AutoMUTEX lock(m_mutex, __TFUNCTION__);

    ServerEntries serverEntryList = GetList();
    if (serverEntryList.size() < 1)
    {
        throw std::exception("No servers found.  This application is possibly corrupt.");
    }

    // The client always tries the first entry in the list.
    // The list will be rearranged elsewhere, such as when a server has failed,
    // or when new servers are discovered.
    return serverEntryList[0];
}

ServerEntries ServerList::GetList()
{
    AutoMUTEX lock(m_mutex, __TFUNCTION__);

    // Load persistent list of servers from system (registry)

    ServerEntries systemServerEntryList;

    if (!IGNORE_SYSTEM_SERVER_LIST)
    {
        try
        {
            systemServerEntryList = GetListFromSystem();
        }
        catch (std::exception &ex)
        {
            my_print(false, string("Not using corrupt System Server List: ") + ex.what());
        }
    }

    // Add embedded list to system list.
    // Cases:
    // - This may be a new client run on a system with an existing registry entry; we want the new embedded values
    // - This may be the first run, in which case the system list is empty

    ServerEntries embeddedServerEntryList;
    try
    {
        embeddedServerEntryList = GetListFromEmbeddedValues();
        // Randomize this list for load-balancing
        random_shuffle(embeddedServerEntryList.begin(), embeddedServerEntryList.end());
    }
    catch (std::exception &ex)
    {
        string message = string("Corrupt Embedded Server List: ") + ex.what();
        throw std::exception(message.c_str());
    }

    for (ServerEntries::iterator embeddedServerEntry = embeddedServerEntryList.begin();
         embeddedServerEntry != embeddedServerEntryList.end(); ++embeddedServerEntry)
    {
        // Check if we already know about this server
        // We prioritize discovery information, so skip embedded entry entirely when already known
        bool alreadyKnown = false;
        for (ServerEntries::iterator systemServerEntry = systemServerEntryList.begin();
             systemServerEntry != systemServerEntryList.end(); ++systemServerEntry)
        {
            if (embeddedServerEntry->serverAddress == systemServerEntry->serverAddress)
            {
                alreadyKnown = true;
                break;
            }
        }

        if (!alreadyKnown)
        {
            // Insert the new entry as the second entry (if there already is at least one),
            // so that the first entry can continue to be used if it is reachable
            systemServerEntryList.insert(systemServerEntryList.size() > 0 ?
                                            systemServerEntryList.begin() + 1 :
                                            systemServerEntryList.begin(),
                                         *embeddedServerEntry);
        }
    }

    // Write this out immediately, so the next time we'll get it from the system
    // (Also so MarkCurrentServerFailed reads the same list we're returning)
    WriteListToSystem(systemServerEntryList);

    return systemServerEntryList;
}

ServerEntries ServerList::GetListFromEmbeddedValues()
{
    return ParseServerEntries(EMBEDDED_SERVER_LIST);
}

ServerEntries ServerList::GetListFromSystem()
{
    string serverEntryListString;

    if (!ReadRegistryStringValue(LOCAL_SETTINGS_REGISTRY_VALUE_SERVERS, serverEntryListString))
    {
         return ServerEntries();
    }

    return ParseServerEntries(serverEntryListString.c_str());
}

// The errors below throw (preventing any Server connection from starting)
ServerEntries ServerList::ParseServerEntries(const char* serverEntryListString)
{
    ServerEntries serverEntryList;

    stringstream stream(serverEntryListString);
    string item;

    while (getline(stream, item, '\n'))
    {
        ServerEntry entry = ParseServerEntry(item);
        if (entry.webServerCertificate != "None")
        {
            serverEntryList.push_back(entry);
        }
    }

    return serverEntryList;
}

ServerEntry ServerList::ParseServerEntry(const string& serverEntry)
{
    string line = Dehexlify(serverEntry);

    ServerEntry entry;
    entry.FromString(line);

    return entry;
}

// NOTE: This function does not throw because we don't want a failure to prevent a connection attempt.
void ServerList::WriteListToSystem(const ServerEntries& serverEntryList)
{
    string encodedServerEntryList = EncodeServerEntries(serverEntryList);

    WriteRegistryStringValue(LOCAL_SETTINGS_REGISTRY_VALUE_SERVERS, encodedServerEntryList);
}

string ServerList::EncodeServerEntries(const ServerEntries& serverEntryList)
{
    string encodedServerList;
    for (ServerEntryIterator it = serverEntryList.begin(); it != serverEntryList.end(); ++it)
    {
        string stringServerEntry = it->ToString();
        encodedServerList += Hexlify(
                                (const unsigned char*)stringServerEntry.c_str(),
                                stringServerEntry.length()) + "\n";
    }
    return encodedServerList;
}


/***********************************************
ServerEntry members
*/

void ServerEntry::Copy(const ServerEntry& src)
{
    this->serverAddress = src.serverAddress;
    this->webServerPort = src.webServerPort;
    this->webServerSecret = src.webServerSecret;
    this->webServerCertificate = src.webServerCertificate;
    this->sshPort = src.sshPort;
    this->sshUsername = src.sshUsername;
    this->sshPassword = src.sshPassword;
    this->sshHostKey = src.sshHostKey;
    this->sshObfuscatedPort = src.sshObfuscatedPort;
    this->sshObfuscatedKey = src.sshObfuscatedKey;
}

string ServerEntry::ToString() const
{
    stringstream ss;
    
    //
    // Legacy values are simply space-separated strings
    //

    ss << serverAddress << " ";
    ss << webServerPort << " ";
    ss << webServerSecret << " ";
    ss << webServerCertificate << " ";

    //
    // Extended values are JSON-encoded.
    //

    Json::Value entry;
    
    entry["sshPort"] = sshPort;
    entry["sshUsername"] = sshUsername;
    entry["sshPassword"] = sshPassword;
    entry["sshHostKey"] = sshHostKey;
    entry["sshObfuscatedPort"] = sshObfuscatedPort;
    entry["sshObfuscatedKey"] = sshObfuscatedKey;

    Json::FastWriter jsonWriter;
    ss << jsonWriter.write(entry);

    return ss.str();
}

void ServerEntry::FromString(const string& str)
{
    stringstream lineStream(str);
    string lineItem;

    //
    // Legacy values are simply space-separated strings
    //

    if (!getline(lineStream, lineItem, ' '))
    {
        throw std::exception("Server Entries are corrupt: can't parse Server Address");
    }
    serverAddress = lineItem;

    if (!getline(lineStream, lineItem, ' '))
    {
        throw std::exception("Server Entries are corrupt: can't parse Web Server Port");
    }
    webServerPort = atoi(lineItem.c_str());

    if (!getline(lineStream, lineItem, ' '))
    {
        throw std::exception("Server Entries are corrupt: can't parse Web Server Secret");
    }
    webServerSecret = lineItem;

    if (!getline(lineStream, lineItem, ' '))
    {
        throw std::exception("Server Entries are corrupt: can't parse Web Server Certificate");
    }
    webServerCertificate = lineItem;

    //
    // Extended values are JSON-encoded.
    //

    if (!getline(lineStream, lineItem, '\0'))
    {
        my_print(true, _T("%s: Extended JSON values not present"), __TFUNCTION__);
        
        // Assumption: we're not reading into a ServerEntry struct that already
        // has values set. So we're relying on the default values being set by
        // the constructor.
        return;
    }

    Json::Value json_entry;
    Json::Reader reader;
    bool parsingSuccessful = reader.parse(lineItem, json_entry);
    if (!parsingSuccessful)
    {
        string fail = reader.getFormattedErrorMessages();
        my_print(false, _T("%s: Extended JSON parse failed: %S"), __TFUNCTION__, reader.getFormattedErrorMessages().c_str());
        throw std::exception("Server Entries are corrupt: can't parse JSON");
    }

    try
    {
        sshPort = json_entry.get("sshPort", 0).asInt();
        sshUsername = json_entry.get("sshUsername", "").asString();
        sshPassword = json_entry.get("sshPassword", "").asString();
        sshHostKey = json_entry.get("sshHostKey", "").asString();
        sshObfuscatedPort = json_entry.get("sshObfuscatedPort", 0).asInt();
        sshObfuscatedKey = json_entry.get("sshObfuscatedKey", "").asString();
    }
    catch (exception& e)
    {
        my_print(false, _T("%s: Extended JSON parse exception: %S"), __TFUNCTION__, e.what());
        throw std::exception("Server Entries are corrupt: parse JSON exception");
    }
}
