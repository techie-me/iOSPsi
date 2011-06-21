#!/usr/bin/python
#
# Copyright (c) 2011, Psiphon Inc.
# All rights reserved.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import binascii
import os
import shutil
import sys
import tempfile
from collections import namedtuple
import datetime
import socket
import struct
import traceback

import xlrd

# Server doesn't need write modules
try:
    import xlwt
    import xlutils.copy
except ImportError:
    pass

try:
    import GeoIP
except ImportError:
    pass


DB_FILENAME = 'psi_db.xls'
DB_PATH = os.path.join(os.path.dirname(__file__), DB_FILENAME)

CLIENTS_SHEET_NAME = u'Clients'
CLIENTS_SHEET_COLUMNS = u'Client_ID,Propagation_Channels,Notes'.split(',')
Client = namedtuple(u'Client', CLIENTS_SHEET_COLUMNS)

HOSTS_SHEET_NAME = u'Hosts'
HOSTS_SHEET_COLUMNS = u'Host_ID,IP_Address,SSH_Username,SSH_Password,SSH_Host_Key,Notes'.split(',')
Host = namedtuple(u'Host', HOSTS_SHEET_COLUMNS)

SERVERS_SHEET_NAME = u'Servers'
SERVERS_SHEET_COLUMNS = u'Server_ID,Host_ID,IP_Address,Web_Server_Port,Web_Server_Secret,Web_Server_Certificate,Web_Server_Private_Key,Discovery_Client_ID,Discovery_Time_Start,Discovery_Time_End,Notes'.split(',')
Server = namedtuple(u'Server', SERVERS_SHEET_COLUMNS)

SPONSORS_SHEET_NAME = u'Sponsors'
SPONSORS_SHEET_COLUMNS = u'Sponsor_ID,Banner_Filename,Notes'.split(',')
Sponsor = namedtuple(u'Sponsor', SPONSORS_SHEET_COLUMNS)

HOME_PAGES_SHEET_NAME = u'Home_Pages'
HOME_PAGES_SHEET_COLUMNS = u'Sponsor_ID,Region,Home_Page_URL,Notes'.split(',')
Home_Page = namedtuple(u'Home_Page', HOME_PAGES_SHEET_COLUMNS)

VERSIONS_SHEET_NAME = u'Versions'
VERSIONS_SHEET_COLUMNS = u'Client_Version,Notes'.split(',')
Version = namedtuple(u'Version', VERSIONS_SHEET_COLUMNS)

SCHEMA = {
    CLIENTS_SHEET_NAME : (CLIENTS_SHEET_COLUMNS, Client),
    HOSTS_SHEET_NAME : (HOSTS_SHEET_COLUMNS, Host),
    SERVERS_SHEET_NAME : (SERVERS_SHEET_COLUMNS, Server),
    SPONSORS_SHEET_NAME : (SPONSORS_SHEET_COLUMNS, Sponsor),
    HOME_PAGES_SHEET_NAME : (HOME_PAGES_SHEET_COLUMNS, Home_Page),
    VERSIONS_SHEET_NAME : (VERSIONS_SHEET_COLUMNS, Version),
}

get_clients = lambda : read_data(CLIENTS_SHEET_NAME)
get_hosts = lambda : read_data(HOSTS_SHEET_NAME)
get_servers = lambda : read_data(SERVERS_SHEET_NAME)
get_sponsors = lambda : read_data(SPONSORS_SHEET_NAME)
get_home_pages = lambda : read_data(HOME_PAGES_SHEET_NAME)
get_versions = lambda : read_data(VERSIONS_SHEET_NAME)
update_servers = lambda(updates) : update_data(SERVERS_SHEET_NAME, updates)


def read_data(sheet_name, spreadsheet_path=None):
    if spreadsheet_path is None:
        spreadsheet_path = get_db_path()
        
    expected_columns, tupletype = SCHEMA[sheet_name]
    xls = xlrd.open_workbook(spreadsheet_path)
    sheet = xls.sheet_by_name(sheet_name)
    assert([cell.value for cell in sheet.row(0)] == expected_columns)
    data = []
    for i in range(1, sheet.nrows):
        row = sheet.row(i)
        values = []
        for j in range(len(expected_columns)):
            if type(row[j].value) == float: # assume it's a date
                values.append(
                    datetime.datetime(*xlrd.xldate_as_tuple(row[j].value, 0)))
            elif type(row[j].value) == str or len(row[j].value) == 0: # empty value
                values.append(None)
            elif type(row[j].value) == unicode: # string value
                values.append(row[j].value.encode('utf-8'))
            else:
                assert(False)
        data.append(tupletype(*values))
    return data


def update_data(sheet_name, update_values, spreadsheet_path=None):
    if spreadsheet_path is None:
        spreadsheet_path = get_db_path()

    # Update values in the specified sheet.
    #
    # This function reads the existing spreadsheet, updates cells, and writes
    # a new copy of the spreasheet, Using xlrd object to read and locate cells
    # and xlwt to write new values.
    #
    # Each update specifies a target row by a key column value and a set of
    # new values for other columns in that row.
    #
    expected_columns, _ = SCHEMA[sheet_name]
    original_xls = xlrd.open_workbook(spreadsheet_path)
    original_sheet = original_xls.sheet_by_name(sheet_name)
    assert([cell.value for cell in original_sheet.row(0)] == expected_columns)
    # Compute sheet index from original as xlwt doesn't offer lookup by name
    sheet_index = original_xls.sheet_names().index(sheet_name)
    xls = xlutils.copy.copy(original_xls)
    sheet = xls.get_sheet(sheet_index)
    for row_key, row_updates in update_values:
        # Find target row index using original (xlrt) as xlwt is write only
        key_column, key_value = row_key
        key_column_index = expected_columns.index(key_column)
        unicode_key_value = key_value.decode()
        column_values = [cell.value for cell in original_sheet.col_slice(key_column_index)]
        row_index = column_values.index(unicode_key_value)
        # Update columns in target row
        for update_column, update_value in row_updates:
            update_column_index = expected_columns.index(update_column)
            unicode_update_value = update_value.decode()
            sheet.write(row_index, update_column_index, unicode_update_value)
    xls.save(spreadsheet_path)


def test_update_data():
    # NOTE: expects test data as defined in psi_db.xls
    TEST_DB_PATH = 'test.xls'
    shutil.copy(DB_PATH, TEST_DB_PATH)
    try:
        updates = []

        updates.append(
            ((u'IP_Address', '192.168.1.109'),
             [(u'Server_ID', 'engual malet uplore'),
              (u'Web_Server_Secret', '0123456789')]))

        updates.append(
            ((u'IP_Address', '192.168.1.151'),
             [(u'Server_ID', 'hareware zinink randowser'),
              (u'Web_Server_Secret', '9876543210')]))

        update_data(SERVERS_SHEET_NAME,
                    updates,
                    spreadsheet_path=TEST_DB_PATH)

        # Unchanged host sheet
        hosts = read_data(HOSTS_SHEET_NAME,
                          spreadsheet_path=TEST_DB_PATH)

        assert(hosts[0].Host_ID == 'Cartman')

        servers = read_data(SERVERS_SHEET_NAME,
                            spreadsheet_path=TEST_DB_PATH)

        # Unchanged server row
        assert(servers[0].Server_ID == 'ubunix electrows uplore')
        assert(servers[0].IP_Address == '192.168.1.250')
        assert(servers[0].Web_Server_Secret == 'FEDCBA9876543210')

        # Changed server rows
        assert(servers[3].Server_ID == 'engual malet uplore')
        assert(servers[3].IP_Address == '192.168.1.109')
        assert(servers[3].Web_Server_Secret == '0123456789')
        assert(servers[6].Server_ID == 'hareware zinink randowser')
        assert(servers[6].IP_Address == '192.168.1.151')
        assert(servers[6].Web_Server_Secret == '9876543210')

    finally:
        os.remove(TEST_DB_PATH)


def validate_data():
    # read all sheets; if no exception thrown, is valid data
    return [
        get_clients(),
        get_hosts(),
        get_servers(),
        get_sponsors(),
        get_home_pages(),
        get_versions()]


def get_encoded_server_list(client_id, client_ip_address=None, discovery_date=datetime.datetime.now()):
    if not client_ip_address:
        # embedded server list
        # output all servers for client ID with no discovery date
        servers = [server for server in get_servers()
                   if server.Discovery_Client_ID == client_id and not server.Discovery_Time_Start]
    else:
        # discovery case
        # count servers for client ID to be discovered in current date range
        servers = [server for server in get_servers()
                   if server.Discovery_Client_ID == client_id and (
                   server.Discovery_Time_Start is not None and
                   server.Discovery_Time_End is not None and
                   server.Discovery_Time_Start <= discovery_date < server.Discovery_Time_End)]
        # number of IP Address buckets is number of matching servers, so just
        # give the client the one server in their bucket
        # NOTE: when there are many servers, we could return more than one per bucket. For example,
        # with 4 matching servers, we could make 2 buckets of 2. But if we have that many servers,
        # it would be better to mix in an additional strategy instead of discovering extra servers
        # for no additional "effort".
        bucket_count = len(servers)
        if bucket_count == 0:
            return []
        bucket = struct.unpack('!L',socket.inet_aton(client_ip_address))[0] % bucket_count
        servers = [servers[bucket]]
    return [binascii.hexlify('%s %s %s %s' %
                         (server.IP_Address,
                          server.Web_Server_Port,
                          server.Web_Server_Secret,
                          server.Web_Server_Certificate))
            for server in servers]


def test_get_encoded_server_list():
    # NOTE: expects test data as defined in psi_db.xls
    # unknown client ID
    assert(len(get_encoded_server_list('')) == 0)
    # embedded case, known client ID
    assert(len(get_encoded_server_list('3A885577DD84EF13')) == 1)
    # discovery case
    week1 = datetime.datetime(2011, 05, 16)
    assert(len(get_encoded_server_list('3A885577DD84EF13', '127.0.0.1', discovery_date=week1)) == 1)
    assert(len(get_encoded_server_list('3A885577DD84EF13', '127.0.0.2', discovery_date=week1)) == 1)
    # different IP address buckets
    assert(get_encoded_server_list('3A885577DD84EF13', '127.0.0.2', discovery_date=week1) !=
           get_encoded_server_list('3A885577DD84EF13', '127.0.0.1', discovery_date=week1))


def get_region(client_ip_address):
    try:
        # NOTE: if the commercial "city" database is available, swap in
        # the following lines to use it instead:
        #
        # file = '/usr/local/share/GeoIP/GeoIPCity.dat'
        # return GeoIP.open(file, GeoIP.GEOIP_MEMORY_CACHE).record_by_name(client_ip_address)['country_code']
        #
        return GeoIP.new(GeoIP.GEOIP_MEMORY_CACHE).country_code_by_name(client_ip_address)
    except NameError:
        return 'None'


def get_sponsor_home_pages(sponsor_id, client_ip_address, region=None):
    home_pages = get_home_pages()
    if not region:
        region = get_region(client_ip_address)
    # case: lookup succeeded and corresponding region home page found
    sponsor_home_pages = [
        home_page.Home_Page_URL for home_page in home_pages if
        home_page.Sponsor_ID == sponsor_id and home_page.Region == region]
    # case: lookup failed or no corresponding region home page found --> use default
    if len(sponsor_home_pages) == 0:
        sponsor_home_pages = [
            home_page.Home_Page_URL for home_page in home_pages if
            home_page.Sponsor_ID == sponsor_id and home_page.Region is None]
    return sponsor_home_pages


def test_get_sponsor_home_pages():
    # NOTE: expects test data as defined in psi_db.xls
    assert(len(get_sponsor_home_pages('', None, None)) == 0)
    # multiple home pages
    assert(len(get_sponsor_home_pages('8BB28C1A8E8A9ED9', None, 'CA')) == 2)
    # default region
    assert(len(get_sponsor_home_pages('8BB28C1A8E8A9ED9', None, 'IR')) == 1)
    assert(len(get_sponsor_home_pages('8BB28C1A8E8A9ED9', None, 'None')) == 1)
    assert(len(get_sponsor_home_pages('8BB28C1A8E8A9ED9', None, '')) == 1)
    # different pages for different sponsors
    assert(len(get_sponsor_home_pages('8BB28C1A8E8A9ED9', None, 'US')) == 1)
    assert(len(get_sponsor_home_pages('6C519A29C9B64E58', None, 'US')) == 1)
    assert(get_sponsor_home_pages('8BB28C1A8E8A9ED9', None, 'US') !=
           get_sponsor_home_pages('6C519A29C9B64E58', None, 'US'))


def get_upgrade(client_version):
    # check last version number against client version number
    # assumes Versions list is in ascending version order
    last_version = get_versions()[-1]
    if len(last_version) < 1:
        return None
    if int(last_version.Client_Version) > int(client_version):
        return last_version.Client_Version
    return None


def test_get_upgrade():
    # NOTE: expects test data as defined in psi_db.xls
    assert(get_upgrade('1') == '2')
    assert(get_upgrade('2') is None)


def handshake(client_ip_address, client_id, sponsor_id, client_version):
    output = []
    homepage_urls = get_sponsor_home_pages(sponsor_id, client_ip_address)
    for homepage_url in homepage_urls:
        output.append('Homepage: %s' % (homepage_url,))
    upgrade_client_version = get_upgrade(client_version)
    if upgrade_client_version:
        output.append('Upgrade: %s' % (upgrade_client_version,))
    for encoded_server_entry in get_encoded_server_list(client_id, client_ip_address):
        output.append('Server: %s' % (encoded_server_entry,))
    return output


def embed(client_id):
    return get_encoded_server_list(client_id)


def get_discovery_client_ids_for_host(host_id, discovery_date=datetime.datetime.now()):
    servers = get_servers()
    servers_on_host = filter(lambda x : x.Host_ID == host_id, servers)
    return set([server.Discovery_Client_ID for server in servers_on_host])


def make_file_for_host(host_id, filename, discovery_date=datetime.datetime.now()):
    # Create a compartmentalized spreadsheet with only the information needed by a particular host
    # - always omit Notes column
    # - client sheet includes only clients that may connect to servers on this host
    # - OMIT host sheet
    # - servers sheet includes only servers for client IDs in filtered client sheet
    #   (which is more than just servers on this host, due to cross-host discovery)
    #   also, omit non-propagation servers not on this host whose discovery time period has elapsed
    #   also, omit propagation servers not on this host
    #   (not on this host --> because servers on this host still need to run, even if not discoverable)
    # - OMIT sponsors sheet
    # - send entire Home Pages sheet
    # - send entire Versions sheet

    wb = xlwt.Workbook()

    # NOTE: in the present implementation, the following reads are non-atomic as each
    # function call re-reads the spreadsheet. This isn't an issue at the moment since
    # the deploy step is invoked manually.

    clients = get_clients()
    servers = get_servers()
    home_pages = get_home_pages()
    versions = get_versions()

    date_style = xlwt.easyxf(num_format_str='YYYY-MM-DD')

    discovery_client_ids_on_host = get_discovery_client_ids_for_host(host_id)

    ws = wb.add_sheet(CLIENTS_SHEET_NAME)
    for i, value in enumerate(CLIENTS_SHEET_COLUMNS):
        ws.write(0, i, value)
    i = 1
    for client in clients:
        if client.Client_ID in discovery_client_ids_on_host:
            ws.write(i, 0, client.Client_ID)
            ws.write(i, 1, '') # Propagation_Channel
            ws.write(i, 2, '') # Notes
            i += 1

    ws = wb.add_sheet(SERVERS_SHEET_NAME)
    for i, value in enumerate(SERVERS_SHEET_COLUMNS):
        ws.write(0, i, value)
    i = 1
    for server in servers:
        if (server.Discovery_Client_ID in discovery_client_ids_on_host and
                not(server.Discovery_Time_Start and server.Host_ID != host_id and server.Discovery_Time_End <= discovery_date) and
                not(server.Discovery_Time_Start is None and server.Host_ID != host_id)):
            ws.write(i, 0, '') # Host_ID
            ws.write(i, 1, server.IP_Address)
            ws.write(i, 2, server.Web_Server_Port)
            ws.write(i, 3, server.Web_Server_Secret)
            ws.write(i, 4, server.Web_Server_Certificate)
            ws.write(i, 5, server.Web_Server_Private_Key)
            ws.write(i, 6, server.Discovery_Client_ID)
            ws.write(i, 7, server.Discovery_Time_Start, date_style)
            ws.write(i, 8, server.Discovery_Time_End, date_style)
            ws.write(i, 9, '') # Notes
            i += 1

    ws = wb.add_sheet(HOME_PAGES_SHEET_NAME)
    for i, value in enumerate(HOME_PAGES_SHEET_COLUMNS):
        ws.write(0, i, value)
    for i, home_page in enumerate(home_pages):
        ws.write(i+1, 0, home_page.Sponsor_ID)
        ws.write(i+1, 1, home_page.Region)
        ws.write(i+1, 2, home_page.Home_Page_URL)
        ws.write(i+1, 3, '') # Notes

    ws = wb.add_sheet(VERSIONS_SHEET_NAME)
    for i, value in enumerate(VERSIONS_SHEET_COLUMNS):
        ws.write(0, i, value)
    for i, version in enumerate(versions):
        ws.write(i+1, 0, version.Client_Version)
        ws.write(i+1, 1, '') # Notes

    wb.save(filename)


def test_make_file_for_host():
    hosts = get_hosts()
    week1 = datetime.datetime(2011, 05, 16)
    for host in hosts:
        file = tempfile.NamedTemporaryFile(delete=False)
        make_file_for_host(host.Host_ID, file.name, discovery_date=week1)
        print file.name


def set_db_root(root_path):
    global DB_PATH
    DB_PATH = os.path.join(root_path, DB_FILENAME)


def get_db_path():
    return DB_PATH


if __name__ == "__main__":
    # run tests
    try:
        validate_data()
        test_get_encoded_server_list()
        test_get_sponsor_home_pages()
        test_get_upgrade()
        test_make_file_for_host()
        test_update_data()
    except Exception as e:
        print 'Failed'
        traceback.print_exc()
        raise e
    print 'Succeeded'
