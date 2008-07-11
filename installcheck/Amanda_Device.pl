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
# Contact information: Zmanda Inc, 465 S Mathlida Ave, Suite 300
# Sunnyvale, CA 94086, USA, or: http://www.zmanda.com

use Test::More tests => 237;
use File::Path qw( mkpath rmtree );
use Sys::Hostname;
use strict;

use lib "@amperldir@";
use Installcheck::Config;
use Amanda::Debug;
use Amanda::Device qw( :constants );
use Amanda::Config qw( :getconf :init );
use Amanda::Types;
use Amanda::Paths;
use Amanda::Tests;

my $dev;
my $dev_name;
my ($vtape1, $vtape2);
my ($input_filename, $output_filename) =
    ( "$AMANDA_TMPDIR/input.tmp", "$AMANDA_TMPDIR/output.tmp" );
my $taperoot = "$AMANDA_TMPDIR/Amanda_Device_test_tapes";
my $testconf;
my $queue_fd;

# we'll need some vtapes..
sub mkvtape {
    my ($num) = @_;

    my $mytape = "$taperoot/$num";
    if (-d $mytape) { rmtree($mytape); }
    mkpath("$mytape/data");
    return $mytape;
}


# make up a fake dumpfile_t to write with
my $dumpfile = Amanda::Types::dumpfile_t->new();
$dumpfile->{type} = $Amanda::Types::F_DUMPFILE;
$dumpfile->{datestamp} = "20070102030405";
$dumpfile->{dumplevel} = 0;
$dumpfile->{compressed} = 1;
$dumpfile->{name} = "localhost";
$dumpfile->{disk} = "/home";
$dumpfile->{program} = "INSTALLCHECK";

# function to set up a queue_fd for a filename
sub make_queue_fd {
    my ($filename, $mode) = @_;

    open(my $fd, $mode, $filename) or die("Could not open $filename: $!");
    return $fd, Amanda::Device::queue_fd_t->new(fileno($fd));
}

sub write_file {
    my ($seed, $length, $filenum) = @_;

    Amanda::Tests::write_random_file($seed, $length, $input_filename);

    ok($dev->start_file($dumpfile),
	"start file $filenum")
	or diag($dev->error_or_status());

    is($dev->file(), $filenum,
	"Device has correct filenum");

    my ($input, $queue_fd) = make_queue_fd($input_filename, "<");
    ok($dev->write_from_fd($queue_fd),
	"write some data")
	or diag($dev->error_or_status());
    close($input) or die("Error closing $input_filename");

    if(ok($dev->in_file(),
	"still in_file")) {
	ok($dev->finish_file(),
	    "finish_file")
	    or diag($dev->error_or_status());
    } else {
	pass("not in file, so not calling finish_file");
    }
}

sub verify_file {
    my ($seed, $length, $filenum) = @_;

    ok(my $read_dumpfile = $dev->seek_file($filenum),
	"seek to file $filenum")
	or diag($dev->error_or_status());
    is($dev->file(), $filenum,
	"device is really at file $filenum");
    is($read_dumpfile->{name}, "localhost",
	"header looks vaguely familiar")
	or diag($dev->error_or_status());

    my ($output, $queue_fd) = make_queue_fd($output_filename, ">");
    ok($dev->read_to_fd($queue_fd),
	"read data from file $filenum")
	or diag($dev->error_or_status());
    close($output) or die("Error closing $output_filename");

    ok(Amanda::Tests::verify_random_file($seed, $length, $output_filename, 0),
	"verified file contents");
}

####
## get stuff set up

$testconf = Installcheck::Config->new();
$testconf->write();
config_init($CONFIG_INIT_EXPLICIT_NAME, 'TESTCONF') == $CFGERR_OK
    or die("Could not load configuration");

# put the debug messages somewhere
Amanda::Debug::dbopen("installcheck");

####
## Test errors a little bit

$dev = Amanda::Device->new("foobar:");
isnt($dev->status(), $DEVICE_STATUS_SUCCESS,
    "creation of a bogus 'foobar:' device fails");

$dev = Amanda::Device->new("rait:{{");
isnt($dev->status(), $DEVICE_STATUS_SUCCESS,
    "creation of a bogus 'rait:{{' device fails");

####
## first, test out the 'null' device.

$dev_name = "null:";

$dev = Amanda::Device->new($dev_name);
is($dev->status(), $DEVICE_STATUS_SUCCESS,
    "create null device")
    or diag $dev->error_or_status();
ok($dev->start($ACCESS_WRITE, "NULL1", "19780615010203"),
    "start null device in write mode")
    or diag $dev->error_or_status();

# check some info methods
isnt($dev->write_min_size(), 0,
    "write_min_size > 0 on null device");
isnt($dev->write_max_size(), 0,
    "write_max_size > 0 on null device");
isnt($dev->read_max_size(), 0,
    "read_max_size > 0 on null device");

# try properties
my $plist = $dev->property_list();
ok($plist,
    "got some properties on null device");
is($dev->property_get("canonical_name"), $dev_name,
    "property_get on null device");

# and write a file to it
write_file(0xabcde, 1024*256, 1);

# (don't finish the device, testing the finalize method's cleanup)

####
## Now some full device tests

## VFS device

$vtape1 = mkvtape(1);
$dev_name = "file:$vtape1";

$dev = Amanda::Device->new($dev_name);
is($dev->status(), $DEVICE_STATUS_SUCCESS,
    "$dev_name: create successful")
    or diag($dev->error_or_status());

$dev->read_label();
ok($dev->status() & $DEVICE_STATUS_VOLUME_UNLABELED,
    "initially unlabeled")
    or diag($dev->error_or_status());

ok($dev->start($ACCESS_WRITE, "TESTCONF13", undef),
    "start in write mode")
    or diag($dev->error_or_status());

ok(!($dev->status() & $DEVICE_STATUS_VOLUME_UNLABELED),
    "not unlabeled anymore")
    or diag($dev->error_or_status());

for (my $i = 1; $i <= 3; $i++) {
    write_file(0x2FACE, $dev->write_min_size()*10+17, $i);
}

ok($dev->finish(),
    "finish device after write")
    or diag($dev->error_or_status());

$dev->read_label();
ok(!($dev->status()),
    "no error, at all, from read_label")
    or diag($dev->error_or_status());

# append one more copy, to test ACCESS_APPEND

ok($dev->start($ACCESS_APPEND, undef, undef),
    "start in append mode")
    or diag($dev->error_or_status());

write_file(0xD0ED0E, $dev->write_min_size()*4, 4);

ok($dev->finish(),
    "finish device after append")
    or diag($dev->error_or_status());

# try reading the third file back, creating a new device
# object first, and skipping the read-label step.

$dev = undef;
$dev = Amanda::Device->new($dev_name);
is($dev->status(), $DEVICE_STATUS_SUCCESS,
    "$dev_name: re-create successful")
    or diag($dev->error_or_status());

ok($dev->start($ACCESS_READ, undef, undef),
    "start in read mode")
    or diag($dev->error_or_status());

verify_file(0x2FACE, $dev->write_min_size()*10+17, 3);

ok($dev->finish(),
    "finish device after read")
    or diag($dev->error_or_status());

####
## Test a RAIT device of two vfs devices.

($vtape1, $vtape2) = (mkvtape(1), mkvtape(2));
$dev_name = "rait:{file:$vtape1,file:$vtape2}";

$dev = Amanda::Device->new($dev_name);
is($dev->status(), $DEVICE_STATUS_SUCCESS,
   "$dev_name: create successful")
    or diag($dev->error_or_status());

$dev->read_label();
ok($dev->status() & $DEVICE_STATUS_VOLUME_UNLABELED,
   "initially unlabeled")
    or diag($dev->error_or_status());

ok($dev->start($ACCESS_WRITE, "TESTCONF13", undef),
   "start in write mode")
    or diag($dev->error_or_status());

ok(!($dev->status() & $DEVICE_STATUS_VOLUME_UNLABELED),
   "not unlabeled anymore")
    or diag($dev->error_or_status());

for (my $i = 1; $i <= 3; $i++) {
    write_file(0x2FACE, $dev->write_max_size()*10+17, $i);
}

ok($dev->finish(),
   "finish device after write")
    or diag($dev->error_or_status());

$dev->read_label();
ok(!($dev->status()),
   "no error, at all, from read_label")
    or diag($dev->error_or_status());

# append one more copy, to test ACCESS_APPEND

ok($dev->start($ACCESS_APPEND, undef, undef),
   "start in append mode")
    or diag($dev->error_or_status());

write_file(0xD0ED0E, $dev->write_max_size()*4, 4);

ok($dev->finish(),
   "finish device after append")
    or diag($dev->error_or_status());

# try reading the third file back, creating a new device
# object first, and skipping the read-label step.

$dev = undef;
$dev = Amanda::Device->new($dev_name);
is($dev->status(), $DEVICE_STATUS_SUCCESS,
    "$dev_name: re-create successful")
    or diag($dev->error_or_status());

ok($dev->start($ACCESS_READ, undef, undef),
   "start in read mode")
    or diag($dev->error_or_status());

verify_file(0x2FACE, $dev->write_max_size()*10+17, 3);

ok($dev->finish(),
   "finish device after read")
    or diag($dev->error_or_status());

# corrupt the device somehow and hope it keeps working
rmtree("$taperoot/1");

ok($dev->start($ACCESS_READ, undef, undef),
   "start in read mode after missing volume")
    or diag($dev->error_or_status());

verify_file(0x2FACE, $dev->write_max_size()*10+17, 3);
verify_file(0xD0ED0E, $dev->write_max_size()*4, 4);
verify_file(0x2FACE, $dev->write_max_size()*10+17, 2);

ok($dev->finish(),
   "finish device read after missing volume")
    or diag($dev->error_or_status());

ok(!($dev->start($ACCESS_WRITE, "TESTCONF29", undef)),
   "start in write mode fails with missing volume")
    or diag($dev->error_or_status());

undef $dev;

$dev_name = "rait:{MISSING,file:$vtape2}";
$dev = Amanda::Device->new($dev_name);

ok($dev->start($ACCESS_READ, undef, undef),
   "start in read mode with MISSING")
    or diag($dev->error_or_status());

verify_file(0x2FACE, $dev->write_max_size()*10+17, 3);
verify_file(0xD0ED0E, $dev->write_max_size()*4, 4);
verify_file(0x2FACE, $dev->write_max_size()*10+17, 2);

ok($dev->finish(),
   "finish device read with MISSING")
    or diag($dev->error_or_status());

ok(!($dev->start($ACCESS_WRITE, "TESTCONF29", undef)),
   "start in write mode fails with MISSING")
    or diag($dev->error_or_status());

undef $dev;

# Make two devices with different labels, should get a
# message accordingly.
($vtape1, $vtape2) = (mkvtape(1), mkvtape(2));
my $rait_name = "rait:{file:$vtape1,file:$vtape2}";

my $dev1 = Amanda::Device->new($vtape1);
is($dev1->status(), $DEVICE_STATUS_SUCCESS,
   "$vtape1: Open successful")
    or diag($dev->error_or_status());
$dev1->start($ACCESS_WRITE, "TESTCONF13", undef);
$dev1->finish();
my $dev2 = Amanda::Device->new($vtape2);
is($dev2->status(), $DEVICE_STATUS_SUCCESS,
   "$vtape2: Open successful")
    or diag($dev->error_or_status());
$dev2->start($ACCESS_WRITE, "TESTCONF14", undef);
$dev2->finish();

$dev1 = $dev2 = undef;

$dev = Amanda::Device->new($rait_name);
is($dev->status(), $DEVICE_STATUS_SUCCESS,
   "$rait_name: Open successful")
    or diag($dev->error_or_status());

$dev->read_label();
ok($dev->status() & $DEVICE_STATUS_DEVICE_ERROR,
   "Label mismatch error")
    or diag($dev->error_or_status());

# Test an S3 device if the proper environment variables are set
my $S3_SECRET_KEY = $ENV{'INSTALLCHECK_S3_SECRET_KEY'};
my $S3_ACCESS_KEY = $ENV{'INSTALLCHECK_S3_ACCESS_KEY'};
my $DEVPAY_SECRET_KEY = $ENV{'INSTALLCHECK_DEVPAY_SECRET_KEY'};
my $DEVPAY_ACCESS_KEY = $ENV{'INSTALLCHECK_DEVPAY_ACCESS_KEY'};
my $DEVPAY_USER_TOKEN = $ENV{'INSTALLCHECK_DEVPAY_USER_TOKEN'};

my $run_s3_tests = defined $S3_SECRET_KEY && defined $S3_ACCESS_KEY;
my $run_devpay_tests = defined $DEVPAY_SECRET_KEY && 
    defined $DEVPAY_ACCESS_KEY && $DEVPAY_USER_TOKEN;

my $dev_base_name;
my $hostname  = hostname();

sub s3_make_device($) {
    my $dev_name = shift @_;
    $dev = Amanda::Device->new($dev_name);
    is($dev->status(), $DEVICE_STATUS_SUCCESS,
       "$dev_name: create successful")
        or diag($dev->error_or_status());

    if ($dev_name =~ /^s3:/) {
        # use regular S3 credentials
        ok($dev->property_set('S3_ACCESS_KEY', $S3_ACCESS_KEY),
           "set S3 access key")
        or diag($dev->error_or_status());

        ok($dev->property_set('S3_SECRET_KEY', $S3_SECRET_KEY),
           "set S3 secret key")
            or diag($dev->error_or_status());
    } elsif ($dev_name =~ /^s3zmanda:/) {
        # use S3+DevPay credentials
        ok($dev->property_set('S3_ACCESS_KEY', $DEVPAY_ACCESS_KEY),
           "set S3+DevPay access key")
        or diag($dev->error_or_status());

        ok($dev->property_set('S3_SECRET_KEY', $DEVPAY_SECRET_KEY),
           "set S3+DevPay secret key")
            or diag($dev->error_or_status());

        ok($dev->property_set('S3_USER_TOKEN', $DEVPAY_USER_TOKEN),
           "set S3+DevPay user token")
            or diag($dev->error_or_status());
    } else {
        diag("didn't recognize the device scheme, so no credentials were set");
    }
    return $dev;
}

sub s3_run_main_tests($$) {
    my ($dev_scheme, $base_name) = @_;
    $dev_name = "$dev_scheme:$base_name-$dev_scheme";
    $dev = s3_make_device($dev_name);
    $dev->read_label();
    my $status = $dev->status();
    # this test appears very liberal, but catches the case where setup_handle fails without
    # giving false positives
    ok(($status == $DEVICE_STATUS_SUCCESS) || (($status & $DEVICE_STATUS_VOLUME_UNLABELED) != 0),
       "status is either OK or possibly already labeled")
        or diag($dev->error_or_status());

    ok($dev->start($ACCESS_WRITE, "TESTCONF13", undef),
       "start in write mode")
        or diag($dev->error_or_status());

    ok(!($dev->status() & $DEVICE_STATUS_VOLUME_UNLABELED),
       "it's labeled now")
        or diag($dev->error_or_status());

    for (my $i = 1; $i <= 3; $i++) {
        write_file(0x2FACE, 32768*10, $i);
    }

    ok($dev->finish(),
       "finish device after write")
        or diag($dev->error_or_status());

    $dev->read_label();
    ok(!($dev->status()),
       "no error, at all, from read_label")
	or diag($dev->error_or_status());

    # append one more copy, to test ACCESS_APPEND

    ok($dev->start($ACCESS_APPEND, undef, undef),
       "start in append mode")
        or diag($dev->error_or_status());

    write_file(0xD0ED0E, 32768*10, 4);

    ok($dev->finish(),
       "finish device after append")
        or diag($dev->error_or_status());

    # try reading the third file back

    ok($dev->start($ACCESS_READ, undef, undef),
       "start in read mode")
        or diag($dev->error_or_status());

    verify_file(0x2FACE, 32768*10, 3);

    ok($dev->finish(),
       "finish device after read")
        or diag($dev->error_or_status());    # (note: we don't use write_max_size here, as the maximum for S3 is very large)

    # try a constrained bucket
    $dev_name = lc("$dev_scheme:$base_name-$dev_scheme-eu");
    $dev = s3_make_device($dev_name);
    ok($dev->property_set('S3_BUCKET_LOCATION', 'EU'),
       "set S3 bucket location")
        or diag($dev->error_or_status());

    $dev->read_label();
    $status = $dev->status();
    ok(($status == $DEVICE_STATUS_SUCCESS) || (($status & $DEVICE_STATUS_VOLUME_UNLABELED) != 0),
       "status is either OK or possibly already labeled")
        or diag($dev->error_or_status());

    # bucket name incompatible with location constraint
    $dev_name = "$dev_scheme:-$base_name-$dev_scheme-eu";
    $dev = s3_make_device($dev_name);

    ok(!$dev->property_set('S3_BUCKET_LOCATION', 'EU'),
       "should not be able to set S3 bucket location with an incompatible name")
        or diag($dev->error_or_status());
}

SKIP: {
    skip "define \$INSTALLCHECK_S3_{SECRET,ACCESS}_KEY to run S3 tests", 25
	unless $run_s3_tests;

    # XXX for best results, the bucket should already exist (Amazon doesn't create
    # buckets quickly enough to pass subsequent tests), but should be empty (so that
    # the device appears unlabeled)
    $dev_base_name = "$S3_ACCESS_KEY-installcheck-$hostname";

    s3_run_main_tests('s3', $dev_base_name);

    # can't set user token without devpay
    $dev_name = "s3:$dev_base_name";
    $dev = s3_make_device($dev_name);
    ok(!$dev->property_set('S3_USER_TOKEN', '123'),
       "set user token, but that shouldn't be possible (not using DevPay)")
        or diag($dev->error_or_status());

}

SKIP: {
    my $test_count = $run_s3_tests ? 5 : 49;
    skip "define \$INSTALLCHECK_DEVPAY_{SECRET_KEY,ACCESS_KEY,USER_TOKEN} to run S3+DevPay tests", $test_count
	unless $run_devpay_tests;
    $dev_base_name = "$DEVPAY_ACCESS_KEY-installcheck-$hostname";

    if ($run_s3_tests) {
        # in this case, most of our code has already been exercised
        # just make sure that authentication works as a basic sanity check
        $dev_name = "s3zmanda:$dev_base_name";
        $dev = s3_make_device($dev_name);
        $dev->read_label();
	my $status = $dev->status();
	# this test appears very liberal, but catches the case where setup_handle fails without
	# giving false positives
	ok(($status == $DEVICE_STATUS_SUCCESS) || (($status & $DEVICE_STATUS_VOLUME_UNLABELED) != 0),
	   "status is either OK or possibly already labeled")
            or diag($dev->error_or_status());
    } else {
        s3_run_main_tests('s3zmanda', $dev_base_name);
    }
}

SKIP: {
    skip "excercised S3 driver in S3 checks, not repeating with S3+DevPay", 19
	if $run_s3_tests;
}

# Test a tape device if the proper environment variables are set
my $TAPE_DEVICE = $ENV{'INSTALLCHECK_TAPE_DEVICE'};
my $run_tape_tests = defined $TAPE_DEVICE;
SKIP: {
    skip "define \$INSTALLCHECK_TAPE_DEVICE to run tape tests", 37
	unless $run_tape_tests;

    $dev_name = "tape:$TAPE_DEVICE";
    $dev = Amanda::Device->new($dev_name);
    is($dev->status(), $DEVICE_STATUS_SUCCESS,
	"$dev_name: create successful")
	or diag($dev->error_or_status());

    $dev->read_label();
    ok(!($dev->status() & ~$DEVICE_STATUS_VOLUME_UNLABELED),
	"no error, except possibly unlabeled, from read_label")
	or diag($dev->error_or_status());

    ok($dev->start($ACCESS_WRITE, "TESTCONF13", undef),
	"start in write mode")
	or diag($dev->error_or_status());

    ok(!($dev->status() & $DEVICE_STATUS_VOLUME_UNLABELED),
	"not unlabeled anymore")
	or diag($dev->error_or_status());

    for (my $i = 1; $i <= 3; $i++) {
	write_file(0x2FACE, $dev->write_min_size()*10+17, $i);
    }

    ok($dev->finish(),
	"finish device after write")
	or diag($dev->error_or_status());

    $dev->read_label();
    ok(!($dev->status()),
	"no error, at all, from read_label")
	or diag($dev->error_or_status());

    is($dev->volume_label(), "TESTCONF13",
	"read_label reads the correct label")
	or diag($dev->error_or_status());

    # append one more copy, to test ACCESS_APPEND

    ok($dev->start($ACCESS_APPEND, undef, undef),
	"start in append mode")
	or diag($dev->error_or_status());

    write_file(0xD0ED0E, $dev->write_min_size()*4, 4);

    ok($dev->finish(),
	"finish device after append")
	or diag($dev->error_or_status());

    # try reading the third file back, creating a new device
    # object first, and skipping the read-label step.

    $dev = undef;
    $dev = Amanda::Device->new($dev_name);
    is($dev->status(), $DEVICE_STATUS_SUCCESS,
	"$dev_name: re-create successful")
	or diag($dev->error_or_status());

    ok($dev->start($ACCESS_READ, undef, undef),
	"start in read mode")
	or diag($dev->error_or_status());

    verify_file(0x2FACE, $dev->write_min_size()*10+17, 3);

    ok($dev->finish(),
	"finish device after read")
	or diag($dev->error_or_status());

}

unlink($input_filename);
unlink($output_filename);
rmtree($taperoot);