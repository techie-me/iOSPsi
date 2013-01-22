# Copyright (c) 2012, Psiphon Inc.
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

'''
Periodically checks email-ID-DB. If diagnostic info ID is found in
diagnostic-info-DB, then response email is formatted and sent; entry is
deleted from email-ID-DB. Also cleans up expired email-ID-DB entries.
'''

import yaml
import smtplib
import time

from config import config
import logger
import datastore
import sendmail
import mailformatter


_SLEEP_TIME_SECS = 60


def _email_diagnostic_info_records_iterator():
    '''
    Generator for obtaining email_diagnostic_info records.
    '''
    while True:
        for rec in datastore.get_email_diagnostic_info_iterator():
            yield rec

        datastore.expire_old_email_diagnostic_info_records()

        time.sleep(_SLEEP_TIME_SECS)


def _clean_diagnostic_info_for_yaml_dumping(diagnostic_info):
    '''
    When we pull the `diagnostic_info` out of the database, it has a '_id'
    field added that is a non-safe-YAML-able object. We'll make sure that
    the object is okay to dump.
    Modifies `diagnostic_info`.
    '''
    for key, value in diagnostic_info.iteritems():
        if key.startswith('_'):
            diagnostic_info[key] = str(value)


def go():
    # Retrieve and process email-to-diagnostic-info records.
    # Note that `_email_diagnostic_info_records` throttles itself if/when
    # there are no records immediately available.
    for email_diagnostic_info in _email_diagnostic_info_records_iterator():
        # Check if there is (yet) a corresponding diagnostic info record
        diagnostic_info = datastore.find_diagnostic_info(email_diagnostic_info['diagnostic_info_id'])
        if not diagnostic_info:
            continue

        # Modifies diagnostic_info
        _clean_diagnostic_info_for_yaml_dumping(diagnostic_info)

        # Convert the modified YAML back into a string for emailing.
        diagnostic_info_text = yaml.safe_dump(diagnostic_info,
                                              default_flow_style=False)

        try:
            diagnostic_info_html = mailformatter.format(diagnostic_info)
        except Exception:
            diagnostic_info_html = None

        # If we get to here, then we have a valid diagnostic email.
        # Reply with the decrypted content.
        try:
            sendmail.send(config['smtpServer'],
                          config['smtpPort'],
                          config['emailUsername'],
                          config['emailPassword'],
                          config['emailUsername'],
                          config['decryptedEmailRecipient'],
                          u'Re: %s' % (email_diagnostic_info['email_subject'] or ''),
                          diagnostic_info_text,
                          diagnostic_info_html,
                          email_diagnostic_info['email_id'])
        except smtplib.SMTPException:
            logger.exception()

        # Delete the processed record. (Note that sending the email might have
        # failed, but we're deleting it anyway. This is a debatable decision.)
        datastore.remove_email_diagnostic_info(email_diagnostic_info)
