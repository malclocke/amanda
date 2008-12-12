# Copyright (c) 2005-2008 Zmanda Inc.  All Rights Reserved.
#
# This library is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 2.1 as
# published by the Free Software Foundation.
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
# Contact information: Zmanda Inc, 465 S Mathlida Ave, Suite 300
# Sunnyvale, CA 94086, USA, or: http://www.zmanda.com

package Amanda::Application::Zfs;

use strict;
use warnings;
use Symbol;
use IPC::Open3;
use Amanda::Debug qw( :logging );

=head1 NAME

Amanda::Application::Zfs -- collection of function to use with zfs

=head1 SYNOPSIS

=head1 INTERFACE

=cut

sub zfs_set_value {
    my $self = shift;

    my $action = $_[0];

    if (defined $self->{execute_where} && $self->{execute_where} ne "client") {
	$self->print_to_server_and_die($action, " Script must be run on the client 'execute_where client'", $Amanda::Script_App::ERROR);
    }
    if (!defined $self->{device}) {
	$self->print_to_server_and_die($action, "'--device' is not provided",
                        $Amanda::Script_App::ERROR);
    }
    if ($self->{df_path} ne "df" && !-e $self->{df_path}) {
	$self->print_to_server_and_die($action, "Can't execute DF-PATH '$self->{df_path}' command",
                        $Amanda::Script_App::ERROR);
    }
    if ($self->{zfs_path} ne "zfs" && !-e $self->{zfs_path}) {
        $self->print_to_server_and_die($action, "Can't execute ZFS-PATH '$self->{zfs_path}' command",
                        $Amanda::Script_App::ERROR);
    }

    if ($self->{pfexec} =~ /^YES$/i) {
        $self->{pfexec_cmd} = $self->{pfexec_path};
    }
    if (defined $self->{pfexec_cmd} && $self->{pfexec_cmd} ne "pfexec" && !-e $self->{pfexec_cmd}) {
        $self->print_to_server_and_die($action, "Can't execute PFEXEC-PATH '$self->{pfexec_cmd}' command",
                        $Amanda::Script_App::ERROR);
    }
    if (!defined $self->{pfexec_cmd}) {
        $self->{pfexec_cmd} = "";
    }

    # determine if $self->{device} is a mountpoint or ZFS dataset
    my $cmd = "$self->{pfexec_cmd} $self->{zfs_path} get -H -o value mountpoint $self->{device}";
    debug "running: $cmd";
    my($wtr, $rdr, $err, $pid);
    $err = Symbol::gensym;
    $pid = open3($wtr, $rdr, $err, $cmd);
    close $wtr;
    my $zmountpoint = <$rdr>;
    waitpid $pid, 0;
    close $rdr;
    close $err;

    if ($? == 0) {
        chomp $zmountpoint;

        # zfs dataset supplied
        $self->{filesystem} = $self->{device};

        # check if zfs volume
        if ($zmountpoint ne '-') {
            $self->{mountpoint} = $zmountpoint;
        } else {
            $self->{mountpoint} = undef;
        }
    } else {
        # filesystem, directory or invalid ZFS dataset name
        $cmd = "$self->{df_path} $self->{device}";
        debug "running: $self->{df_path} $self->{device}";
        $err = Symbol::gensym;
        $pid = open3($wtr, $rdr, $err, $cmd);
        close $wtr;
        my $ret = <$rdr>;
        my $errmsg = <$err>;
        waitpid $pid, 0;
        close $rdr;
        close $err;

        if ($? != 0) {
            # invalid filesystem of ZFS dataset name
            if (defined $errmsg) {
                chomp $errmsg;
            }
            if (defined $ret && defined $errmsg) {
                $self->print_to_server_and_die($action, $ret, $errmsg, $Amanda::Script_App::ERROR);
            } elsif (defined $ret) {
                $self->print_to_server_and_die($action, $ret, $Amanda::Script_App::ERROR);
            } elsif (defined $errmsg) {
                $self->print_to_server_and_die($action, $errmsg, $Amanda::Script_App::ERROR);
            } else {
                $self->print_to_server_and_die($action,
                            "Failed to find mount points: $self->{device}",
                            $Amanda::Script_App::ERROR);
            }
        }
        chomp $ret;
        my @ret = split /:/, $ret;
        if ($ret[0] =~ /(\S*)(\s*)(\()(\S*)(\s*)(\))$/) {
            $self->{mountpoint} = $1;
            $self->{filesystem} = $4;
        }

        $cmd = "$self->{pfexec_cmd} $self->{zfs_path} get -H -o value mountpoint $self->{filesystem}";
        debug "running: $cmd|";
        $err = Symbol::gensym;
        $pid = open3($wtr, $rdr, $err, $cmd);
        close $wtr;
        $zmountpoint = <$rdr>;
	chomp $zmountpoint;
        $errmsg = <$err>;
        waitpid $pid, 0;
        close $rdr;
        close $err;

        if ($? != 0) {
            if (defined $errmsg) {
                chomp $errmsg;
            }
            if (defined $zmountpoint && defined $errmsg) {
                $self->print_to_server_and_die($action, $zmountpoint, $errmsg, $Amanda::Script_App::ERROR);
            } elsif (defined $zmountpoint) {
                $self->print_to_server_and_die($action, $zmountpoint, $Amanda::Script_App::ERROR);
            } elsif (defined $errmsg) {
                $self->print_to_server_and_die($action, $errmsg, $Amanda::Script_App::ERROR);
            } else {
                $self->print_to_server_and_die($action,
                        "Failed to find mount points: $self->{filesystem}",
                        $Amanda::Script_App::ERROR);
            }
        }
        if ($zmountpoint ne $self->{mountpoint}) {
            $self->print_to_server_and_die($action,
                "mountpoint from 'df' ($self->{mountpoint}) and 'zfs list' ($zmountpoint) differ",
                $Amanda::Script_App::ERROR);
        }

        if (!($self->{device} =~ /^$self->{mountpoint}/)) {
            $self->print_to_server_and_die($action,
                "mountpoint '$self->{mountpoint}' is not a prefix of diskdevice '$self->{device}'",
                $Amanda::Script_App::ERROR);
        }

    }

    $self->{snapshot} = Amanda::Util::sanitise_filename($self->{device});
    if (defined $self->{mountpoint}) {
	if ($self->{device} =~ /^$self->{mountpoint}/) {
            $self->{dir} = $self->{device};
            $self->{dir} =~ s,^$self->{mountpoint},,;
            $self->{directory} = $self->{mountpoint} . "/.zfs/snapshot/" .
                                 $self->{snapshot} . $self->{dir};
	} else { # device is not the mountpoint
	    $self->{directory} = $self->{mountpoint} . "/.zfs/snapshot/" .
				 $self->{snapshot};
	}
    }
}

sub zfs_create_snapshot {
    my $self = shift;
    my $action = shift;

    my $cmd = "$self->{pfexec_cmd} $self->{zfs_path} snapshot $self->{filesystem}\@$self->{snapshot}";
    debug "running: $cmd";
    my($wtr, $rdr, $err, $pid);
    $err = Symbol::gensym;
    $pid = open3($wtr, $rdr, $err, $cmd);
    close $wtr;
    my ($msg) = <$rdr>;
    my ($errmsg) = <$err>;
    waitpid $pid, 0;
    close $rdr;
    close $err;
    if( $? != 0 ) {
        if(defined $msg && defined $errmsg) {
            $self->print_to_server_and_die($action, "$msg, $errmsg", $Amanda::Script_App::ERROR);
        } elsif (defined $msg) {
            $self->print_to_server_and_die($action, $msg, $Amanda::Script_App::ERROR);
        } elsif (defined $errmsg) {
            $self->print_to_server_and_die($action, $errmsg, $Amanda::Script_App::ERROR);
        } else {
            $self->print_to_server_and_die($action, "cannot create snapshot '$self->{filesystem}\@$self->{snapshot}': unknown reason", $Amanda::Script_App::ERROR);
        }
    }
}

sub zfs_destroy_snapshot {
    my $self = shift;
    my $action = shift;

    my $cmd = "$self->{pfexec_cmd} $self->{zfs_path} destroy $self->{filesystem}\@$self->{snapshot}";
    debug "running: $cmd|";
    my($wtr, $rdr, $err, $pid);
    my($msg, $errmsg);
    $err = Symbol::gensym;
    $pid = open3($wtr, $rdr, $err, $cmd);
    close $wtr;
    $msg = <$rdr>;
    $errmsg = <$err>;
    waitpid $pid, 0;
    close $rdr;
    close $err;
    if( $? != 0 ) {
        if(defined $msg && defined $errmsg) {
            $self->print_to_server_and_die($action, "$msg, $errmsg", $Amanda::Script_App::ERROR);
        } elsif (defined $msg) {
            $self->print_to_server_and_die($action, $msg, $Amanda::Script_App::ERROR);
        } elsif (defined $errmsg) {
            $self->print_to_server_and_die($action, $errmsg, $Amanda::Script_App::ERROR);
        } else {
            $self->print_to_server_and_die($action, "cannot destroy snapshot '$self->{filesystem}\@$self->{snapshot}': unknown reason", $Amanda::Script_App::ERROR);
        }
    }
}

1;