#!/usr/bin/perl
# qdev-host guest hookscript for Proxmox VE: starts the VM's qdev-host device
# processes and hot-plugs them after the VM started, stops them after it stopped.
# Installed into a snippets storage by qdevctl; chains the VM's previous
# hookscript, if qdevctl recorded one.
use strict;
use warnings;

my ($vmid, $phase) = @ARGV;
die "usage: $0 VMID PHASE\n" unless defined $vmid && defined $phase;

system('/usr/sbin/qdevctl', 'hook', $vmid, $phase);
my $rc = $? >> 8;
if ($phase eq 'post-start' && $rc != 0) {
    print STDERR "qdev-host: device attach for VM $vmid reported errors (rc=$rc)\n";
}

my $conf = "/etc/qdev-host/vms/$vmid.json";
if (-r $conf) {
    open(my $fh, '<', $conf) or exit 0;
    local $/; my $json = <$fh>; close $fh;
    if ($json =~ /"previous_hookscript"\s*:\s*"([^"]+)"/) {
        my $prev = $1;
        if (-x $prev) {
            system($prev, $vmid, $phase);
            exit($? >> 8) if $phase eq 'pre-start';
        }
    }
}
exit 0;
