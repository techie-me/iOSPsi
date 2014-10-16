#!/usr/bin/python

import optparse
import ansible.runner
import ansible.playbook
import os
import sys
import datetime
import psi_ops
import pynliner
import collections

import psi_ops_config

PSI_OPS_DB_FILENAME = os.path.join(os.path.abspath('.'), 'psi_ops.dat')

from mako.template import Template
from mako.lookup import TemplateLookup
from mako import exceptions

# Using the FeedbackDecryptor's mail capabilities
sys.path.append(os.path.abspath(os.path.join('..', 'EmailResponder')))
sys.path.append(os.path.abspath(os.path.join('..', 'EmailResponder', 'FeedbackDecryptor')))
import sender
from config import config

def prepare_linode_base_host(account):
    linode_account = account
    Host = collections.namedtuple('Host', ['id', 'ip_address', 'ssh_username', 'ssh_password', 'ssh_port'])
    base_host = Host('linode_base_image', linode_account.base_ip_address, 'root', 
                     linode_account.base_root_password, linode_account.base_ssh_port)
    base_host = populate_ansible_hosts([base_host])
    return base_host

def create_host(host_name=None, host_vars=dict()):
    """
        Create a new host object and return it.
        host_name: String containing IP/name of server
        host_vars: Variables that are set against the host.
    """
    try:
        # Create a new host entry and set variables
        if isinstance(host_name, basestring):
            host = ansible.inventory.host.Host(host_name)
            
            for k,v in host_vars.iteritems():
                host.set_variable(k,v)
    
    except Exception as e:
        print type(e), str(e)
        raise e
    
    return host

def add_hosts_to_group(hosts, group):
    """
        Add a single or list of Ansible host objects to an Ansible group
        hosts = ansible.inventory.Host
        group = ansible.inventory.group.Group
    """
    try:
        if type(hosts) is ansible.inventory.Host:
            # probably means we only have one host
            group.add_host(hosts)
        elif isinstance(hosts, list):
            for host in hosts:
                group.add_host(host)
    
    except Exception as e:
        print type(e), str(e)
        raise e

def run_against_inventory(inv=ansible.inventory.Inventory([]), 
                          mod_name='ping', mod_args='', pattern='*', forks=10):
    """
        Run a single task against an Inventory.
        inv : Ansible Inventory object
        mod_name : module name
        mod_args : extra arguments for the module
        pattern : hosts or groups to match against
        forks : number of forks for the runner to create (default = 10)
    """
    try:
        # create a runnable task and execute
        runner = ansible.runner.Runner(
            module_name=mod_name,
            module_args=mod_args,
            pattern=pattern,
            forks=forks,
            inventory=inv,
        )
        
        return runner.run()
    
    except Exception as e:
        raise e

def organize_hosts_by_provider(hosts_list):
    """
        Takes a list of psinet hosts and organizes into provider dictionary objects.
        hosts_list : list of psinet hosts
    """
    hosts_dict = dict()
    try:
        all_hosts = hosts_list
        for host in all_hosts:
            if host.provider not in hosts_dict.keys():
                hosts_dict[host.provider] = list()
            
            hosts_dict[host.provider].append(host)
    
    except Exception as e:
        raise e
    
    return hosts_dict

def populate_ansible_hosts(hosts=list()):
    """
        Maps a list of psinet hosts into Ansible Hosts
        hosts : list of psinet hosts
    """
    ansible_hosts = list()
    try:
        for host in hosts:
            ansible_hosts.append(create_host(host_name=host.id, 
                                       host_vars={'ansible_ssh_host': host.ip_address,
                                                  'ansible_ssh_user': host.ssh_username, 
                                                  'ansible_ssh_pass': host.ssh_password,
                                                  'ansible_ssh_port': host.ssh_port,
                                                 }))
            
    except Exception as e:
        raise e
    
    return ansible_hosts

def run_playbook(playbook_file=None, inventory=ansible.inventory.Inventory([]), 
                 verbose=psi_ops_config.ANSIBLE_VERBOSE_LEVEL, email_stats=True):
    """
        Runs a playbook file and returns the result
        playbook_file : Playbook file to open and run (String)
        inventory : Ansible inventory to run playbook against
        verbose : Output verbosity
    """
    try:
        start_time = datetime.datetime.now()
        playbook_callbacks = ansible.callbacks.PlaybookCallbacks(verbose=verbose)
        stats = ansible.callbacks.AggregateStats()
        runner_callbacks = ansible.callbacks.PlaybookRunnerCallbacks(stats, verbose=verbose)
            
        playbook = ansible.playbook.PlayBook(playbook=playbook_file, 
                callbacks=playbook_callbacks, runner_callbacks=runner_callbacks, 
                stats=stats, inventory=inventory)
        
        res = playbook.run()
        end_time = datetime.datetime.now()
        print "Run completed at: %s\nTotal run time: %s" % (str(end_time), str(end_time-start_time))
        
        if email_stats == True:
            # stats.dark : (dict) number of hosts that could not be contacted 
            # stats.failures : (dict) number of hosts that failed to complete the tasks
            (host_output, host_errs) = process_playbook_vars_cache(playbook)
            setup_cache = process_playbook_setup_cache(playbook)
            record = (str(start_time), str(end_time), playbook_file, stats.processed, stats.dark, stats.failures, stats.changed, stats.skipped, res, host_output, host_errs, setup_cache)
            send_mail(record)
        
        return (stats, res)
    
    except Exception as e:
        raise e

def process_playbook_vars_cache(playbook, keywords=['response', 'cmd_result']):
    cache = playbook.VARS_CACHE
    host_errs = dict()
    host_output = dict()
    
    if len(cache) > 0:
        for host in cache:
            keyword = [k for k in keywords if k in cache[host]]
            if len(keyword) == 0:
                return (host_output, host_errs)
            
            keyword = keyword[0]
            
            if keyword in cache[host]:
                if cache[host][keyword]['changed']:
                    if cache[host][keyword]['stderr']:
                        host_errs[host] = cache[host]
                    elif cache[host][keyword]['stdout']:
                        host_output[host] = cache[host]
    return (host_output, host_errs)

def process_playbook_setup_cache(playbook):
    setup_cache = dict()
    
    for host in playbook.SETUP_CACHE:
        setup_cache[host] = playbook.SETUP_CACHE[host]
    return setup_cache

def send_mail(record, subject='PSI Ansible Report', 
              template_filename='psi_mail_ansible_stats.mako'):
    
    if not os.path.isfile(template_filename):
        raise
    
    template_lookup = TemplateLookup(directories=[os.path.dirname(os.path.abspath('__file__'))])
    template = Template(filename=template_filename, default_filters=['unicode', 'h'], lookup=template_lookup)
    
    try:
        rendered = template.render(data=record)
    except:
        raise Exception(exceptions.text_error_template().render())
    
    # CSS in email HTML must be inline
    rendered = pynliner.fromString(rendered)
    
    sender.send(config['emailRecipients'], config['emailUsername'], subject, repr(record), rendered)

def refresh_base_images(providers=['linode']):
    """
        Updates providers base images
        providers : a list of providers so they can be selectively updated
    """
    try:
        psinet = psi_ops.PsiphonNetwork.load_from_file(PSI_OPS_DB_FILENAME)
        linode_base_host = prepare_linode_base_host(psinet._PsiphonNetwork__linode_account)
        
        inv = ansible.inventory.Inventory([])
        
        for provider_name in providers:
            group = ansible.inventory.Group(provider_name)
            add_hosts_to_group(linode_base_host, group)
            inv.add_group(group)
        
        (stats, res) = run_playbook(playbook_file='ansible/update_base_images.yml', inventory=inv, email_stats=True)
        
    except Exception as e:
        raise e

def update_dat():
    """
        Calls external script to update dat file.
    """
    print "Updating psi_ops.dat"
    import psi_update_dat
    psi_update_dat.main()
    

def main(infile=None, send_mail_stats=False):
    try:
        psinet = psi_ops.PsiphonNetwork.load_from_file(PSI_OPS_DB_FILENAME)
        psinet_hosts_list = psinet.get_hosts()
                
        inv = ansible.inventory.Inventory([])

        # Add test group if set
        if psi_ops_config.ANSIBLE_INCLUDE_TEST_GROUP == True:
            print "Creating Test Group"
            test_hosts_list = list()
            for h in psinet_hosts_list:
                if h.id in psi_ops_config.ANSIBLE_TEST_HOSTS:
                    test_hosts_list.append(h)
            
            ansible_hosts_list = populate_ansible_hosts(test_hosts_list)
            group = ansible.inventory.Group(psi_ops_config.ANSIBLE_TEST_GROUP)
            add_hosts_to_group(ansible_hosts_list, group)
            inv.add_group(group)

        #Run against subset
        if psi_ops_config.RUN_AGAINST_SUBSET == True:
            print 'Running Playbook against subset'
            subset_hosts_list = psi_ops_config.ANSIBLE_TEST_DIGITALOCEAN + psi_ops_config.ANSIBLE_TEST_LINODES + psi_ops_config.ANSIBLE_TEST_FASTHOSTS
            psinet_hosts_list = [h for h in psinet_hosts_list if h.id in subset_hosts_list]

        psinet_hosts_dict = organize_hosts_by_provider(psinet_hosts_list)

        for provider in psinet_hosts_dict:
            group = ansible.inventory.Group(provider)
            ansible_hosts_list = populate_ansible_hosts(psinet_hosts_dict[provider])
            add_hosts_to_group(ansible_hosts_list, group)
            inv.add_group(group)

        # Add linode base image group
        if psi_ops_config.ANSIBLE_INCLUDE_BASE_IMAGE == True:
            print "Creating Linode Base Image Group"
            linode_base_host = prepare_linode_base_host(psinet._PsiphonNetwork__linode_account)
            group = ansible.inventory.Group('linode_base_image')
            add_hosts_to_group(linode_base_host, group)
            inv.add_group(group)

        if not infile: 
            raise "Must specify input file" 
        elif os.path.isfile(infile):
            playbook_file = infile
            (stats, res) = run_playbook(playbook_file, inv, send_mail_stats)
        
    except Exception as e:
        raise type(e), str(e)


if __name__ == "__main__":
    parser = optparse.OptionParser('usage: %prog [options]')
    parser.add_option("-i", "--infile", help="Specify ansible playbook file")
    parser.add_option("-t", "--test_servers", action="store_true", help="Runs playbook against test systems")
    parser.add_option("-s", "--subset", action="store_true", help="Run against a subset of servers")
    parser.add_option("-b", "--base_image", action="store_true", help="Forces base image to be included")
    parser.add_option("-r", "--refresh_base_images", action="store_true", help="Updates base images for linode and digitalocean")
    parser.add_option("-m", "--send_mail", action="store_true", help="Send email after playbook is run")
    parser.add_option("-u", "--update_dat", action="store_true", help="Update dat file")
    
    infile=None
    send_mail_stats = False
    
    (options, _) = parser.parse_args()
    
    if options.send_mail:
        send_mail_stats=True
    if options.test_servers:
        psi_ops_config.ANSIBLE_INCLUDE_TEST_GROUP = True
    if options.subset:
        psi_ops_config.RUN_AGAINST_SUBSET = True
    if options.base_image:
        psi_ops_config.ANSIBLE_INCLUDE_BASE_IMAGE = True
    
    if options.update_dat:
        update_dat()
    
    if options.refresh_base_images:
        refresh_base_images(['Testing'])
        exit(0)
        
    if options.infile:
        infile = options.infile
        print infile
        main(infile=infile, send_mail_stats=send_mail_stats)
    


