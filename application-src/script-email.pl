#!@PERL@ -T
# Copyright (c) 2005-2008 Zmanda Inc.  All Rights Reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as published
# by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
#
# Contact information: Zmanda Inc., 465 S Mathlida Ave, Suite 300
# Sunnyvale, CA 94086, USA, or: http://www.zmanda.com

# Run perl.
eval '(exit $?0)' && eval 'exec @PERL@ -S $0 ${1+"$@"}'
	& eval 'exec @PERL@ -S $0 $argv:q'
		if 0;

require "newgetopt.pl";

delete @ENV{'IFS', 'CDPATH', 'ENV', 'BASH_ENV', 'PATH'};
$ENV{'PATH'} = "/usr/bin:/usr/sbin:/sbin:/bin";

$debug=1;
push(@INC, "@APPLICATION_DIR@");

use File::Copy;
use IPC::Open3;
use Sys::Hostname;

open(DEBUG,">>@AMANDA_DBGDIR@/script-email.$$.debug") if ($debug==1);
print DEBUG "program: $0\n" if ($debug==1);

$prefix='@prefix@';
$prefix = $prefix;
$exec_prefix="@exec_prefix@";
$exec_prefix=$exec_prefix;
$libexecdir="@libexecdir@";
$libexecdir=$libexecdir;
$application_dir = '@APPLICATION_DIR@';

$USE_VERSION_SUFFIXES='@USE_VERSION_SUFFIXES@';
$suf = '';
if ( $USE_VERSION_SUFFIXES =~ /^yes$/i ) {
   $suf='-@VERSION@';
}

$myhost = hostname;
$myhost =~ s/\..*$//;
$mailer = '@DEFAULT_MAILER@';

$has_config   = 1;
$has_host     = 1;
$has_disk     = 1;

sub command_support {
   my($config, $host, $disk, $device, @level) = @_;
   print "CONFIG YES\n";
   print DEBUG "STDOUT: CONFIG YES\n" if ($debug == 1);
   print "HOST YES\n";
   print DEBUG "STDOUT: HOST YES\n" if ($debug == 1);
   print "DISK YES\n";
   print DEBUG "STDOUT: DISK YES\n" if ($debug == 1);
   print "MAX-LEVEL 9\n";
   print DEBUG "STDOUT: MAX-LEVEL 9\n" if ($debug == 1);
   print "MESSAGE-LINE YES\n";
   print DEBUG "STDOUT: MESSAGE-LINE YES\n" if ($debug == 1);
   print "MESSAGE-XML NO\n";
   print DEBUG "STDOUT: MESSAGE-XML NO\n" if ($debug == 1);
}

sub command_pre_dle_amcheck {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-dle-amcheck", $config, $host, $disk, $device, @level);
}

sub command_pre_host_amcheck {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-host-amcheck", $config, $host, $disk, $device, @level);
}

sub command_post_dle_amcheck {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-dle-amcheck", $config, $host, $disk, $device, @level);
}

sub command_post_host_amcheck {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-host-amcheck", $config, $host, $disk, $device, @level);
}

sub command_pre_dle_estimate {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-dle-estimate", $config, $host, $disk, $device, @level);
}

sub command_pre_host_estimate {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-host-estimate", $config, $host, $disk, $device, @level);
}

sub command_post_dle_estimate {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-dle-estimate", $config, $host, $disk, $device, @level);
}

sub command_post_host_estimate {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-host-estimate", $config, $host, $disk, $device, @level);
}

sub command_pre_dle_backup {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-dle-backup", $config, $host, $disk, $device, @level);
}

sub command_pre_host_backup {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-host-backup", $config, $host, $disk, $device, @level);
}

sub command_post_dle_backup {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-dle-backup", $config, $host, $disk, $device, @level);
}

sub command_post_host_backup {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-host-backup", $config, $host, $disk, $device, @level);
}

sub command_pre_recover {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-recover", $config, $host, $disk, $device, @level);
}

sub command_post_recover {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-recover", $config, $host, $disk, $device, @level);
}

sub command_pre_level_recover {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("pre-level-recover", $config, $host, $disk, $device, @level);
}

sub command_post_level_recover {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("post-level-recover", $config, $host, $disk, $device, @level);
}

sub command_inter_level_recover {
   my($config, $host, $disk, $device, @level) = @_;
   sendmail("inter-level-recover", $config, $host, $disk, $device, @level);
}

sub sendmail {
   my($function, $config, $host, $disk, $device, @level) = @_;
   if (defined(@opt_mailto)) {
      $destcheck = join ',', @opt_mailto;
      $destcheck =~ /^([a-zA-Z,]*)$/;
      $dest = $1;
   } else {
      $dest = "root";
   }
   $cmd = "$mailer -s \"$config $function $host $disk $device " . join (" ", @level) ." \" $dest";
   print DEBUG "cmd: $cmd\n" if ($debug == 1);
   open(MAIL,"|$cmd");
   print MAIL "$config $function $host $disk $device ", join (" ", @level), "\n";
   close MAIL;
}

$result = &NGetOpt ("config=s", "host=s", "disk=s", "device=s", "level=s@", "index=s", "message=s", "collection", "record", "mailto=s@");
$result = $result;

require "$application_dir/generic-script"