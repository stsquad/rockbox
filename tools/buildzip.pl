#!/usr/bin/perl
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#

use strict;

use File::Copy; # For move() and copy()
use File::Find; # For find()
use File::Path; # For rmtree()
use Cwd 'abs_path';
use Getopt::Long qw(:config pass_through);	# pass_through so not confused by -DTYPE_STUFF

my $ROOT="..";

my $ziptool="zip -r9";
my $output="rockbox.zip";
my $verbose;
my $sim;
my $exe;
my $target;
my $archos;
my $incfonts;
my $target_id; # passed in, not currently used
my $rockbox_root=".rockbox"; # can be changed for special builds



sub glob_copy {
    my ($pattern, $destination) = @_;
    print "glob_copy: $pattern -> $destination\n" if $verbose;
    foreach my $path (glob($pattern)) {
        copy($path, $destination);
    }
}

sub glob_move {
    my ($pattern, $destination) = @_;
    print "glob_move: $pattern -> $destination\n" if $verbose;
    foreach my $path (glob($pattern)) {
        move($path, $destination);
    }
}

sub glob_unlink {
    my ($pattern) = @_;
    print "glob_unlink: $pattern\n" if $verbose;
    foreach my $path (glob($pattern)) {
        unlink($path);
    }
}

sub find_copyfile {
    my ($pattern, $destination) = @_;
    print "find_copyfile: $pattern -> $destination\n" if $verbose;
    return sub {
        my $path = $_;
        if ($path =~ $pattern && filesize($path) > 0 && !($path =~ /$rockbox_root/)) {
            copy($path, $destination);
            chmod(0755, $destination.'/'.$path);
        }
    }
}

# Get options
GetOptions ( 'r|root=s'		=> \$ROOT,
	     'z|ziptool=s'	=> \$ziptool,
	     't|target=s'	=> \$archos,     # The target name as used in ARCHOS in the root makefile
	     'i|id=s'		=> \$target_id,  # The target id name as used in TARGET_ID in the root makefile
	     'o|output=s'	=> \$output,
	     'f|fonts=s'	=> \$incfonts,   # 0 - no fonts, 1 - fonts only 2 - fonts and package
	     'v|verbose'	=> \$verbose,
	     's|sim'		=> \$sim,
	     'rockroot=s'       => \$rockbox_root, # If we want to put in a different directory
    );

($target, $exe) = @ARGV;

# Some basic sanity
die "No firmware found @ $exe" if !-f $exe;

my $firmdir="$ROOT/firmware";
my $appsdir="$ROOT/apps";
my $viewer_bmpdir="$ROOT/apps/plugins/bitmaps/viewer_defaults";

my $cppdef = $target;

sub gettargetinfo {
    open(GCC, ">gcctemp");
    # Get the LCD screen depth and graphical status
    print GCC <<STOP
\#include "config.h"
#ifdef HAVE_LCD_BITMAP
Bitmap: yes
Depth: LCD_DEPTH
Icon Width: CONFIG_DEFAULT_ICON_WIDTH
Icon Height: CONFIG_DEFAULT_ICON_HEIGHT
#endif
Codec: CONFIG_CODEC
#ifdef HAVE_REMOTE_LCD
Remote Depth: LCD_REMOTE_DEPTH
Remote Icon Width: CONFIG_REMOTE_DEFAULT_ICON_WIDTH
Remote Icon Height: CONFIG_REMOTE_DEFAULT_ICON_HEIGHT
#else
Remote Depth: 0
#endif
#ifdef HAVE_RECORDING
Recording: yes
#endif
STOP
;
    close(GCC);

    my $c="cat gcctemp | gcc $cppdef -I. -I$firmdir/export -E -P -";

    # print STDERR "CMD $c\n";

    open(TARGET, "$c|");

    my ($bitmap, $depth, $swcodec, $icon_h, $icon_w);
    my ($remote_depth, $remote_icon_h, $remote_icon_w);
    my ($recording);
    my $icon_count = 1;
    while(<TARGET>) {
        # print STDERR "DATA: $_";
        if($_ =~ /^Bitmap: (.*)/) {
            $bitmap = $1;
        }
        elsif($_ =~ /^Depth: (\d*)/) {
            $depth = $1;
        }
        elsif($_ =~ /^Icon Width: (\d*)/) {
            $icon_w = $1;
        }
        elsif($_ =~ /^Icon Height: (\d*)/) {
            $icon_h = $1;
        }
        elsif($_ =~ /^Codec: (\d*)/) {
            # SWCODEC is 1, the others are HWCODEC
            $swcodec = ($1 == 1);
        }
        elsif($_ =~ /^Remote Depth: (\d*)/) {
            $remote_depth = $1;
        }
        elsif($_ =~ /^Remote Icon Width: (\d*)/) {
            $remote_icon_w = $1;
        }
        elsif($_ =~ /^Remote Icon Height: (\d*)/) {
            $remote_icon_h = $1;
        }
        if($_ =~ /^Recording: (.*)/) {
            $recording = $1;
        }
    }
    close(TARGET);
    unlink("gcctemp");

    return ($bitmap, $depth, $icon_w, $icon_h, $recording,
            $swcodec, $remote_depth, $remote_icon_w, $remote_icon_h);
}

sub filesize {
    my ($filename)=@_;
    my ($dev,$ino,$mode,$nlink,$uid,$gid,$rdev,$size,
        $atime,$mtime,$ctime,$blksize,$blocks)
        = stat($filename);
    return $size;
}

sub buildzip {
    my ($image, $fonts)=@_;

    print "buildzip: image=$image fonts=$fonts\n" if $verbose;
    
    my ($bitmap, $depth, $icon_w, $icon_h, $recording, $swcodec,
        $remote_depth, $remote_icon_w, $remote_icon_h) = &gettargetinfo();

    # print "Bitmap: $bitmap\nDepth: $depth\nSwcodec: $swcodec\n";

    # remove old traces
    rmtree('$rockbox_root');

    mkdir "$rockbox_root", 0777;

    if(!$bitmap) {
        # always disable fonts on non-bitmap targets
        $fonts = 0;
    }
    if($fonts) {
        mkdir "$rockbox_root/fonts", 0777;
        chdir("$rockbox_root/fonts");
        my $cmd = "$ROOT/tools/convbdf -f $ROOT/fonts/*bdf >/dev/null 2>&1";
        print($cmd."\n") if $verbose;
        system($cmd);
        chdir("../../");

        if($fonts < 2) {
          # fonts-only package, return
          return;
        }
    }

    # create the file so the database does not try indexing a folder
    open(IGNORE, ">$rockbox_root/database.ignore")  || die "can't open database.ignore";
    close(IGNORE);
    
    mkdir "$rockbox_root/langs", 0777;
    mkdir "$rockbox_root/rocks", 0777;
    mkdir "$rockbox_root/rocks/games", 0777;
    mkdir "$rockbox_root/rocks/apps", 0777;
    mkdir "$rockbox_root/rocks/demos", 0777;
    mkdir "$rockbox_root/rocks/viewers", 0777;

    if ($recording) {
        mkdir "$rockbox_root/recpresets", 0777;
    }

    if($swcodec) {
        mkdir "$rockbox_root/eqs", 0777;

        glob_copy("$ROOT/apps/eqs/*.cfg", "$rockbox_root/eqs/"); # equalizer presets
    }

    mkdir "$rockbox_root/wps", 0777;
    mkdir "$rockbox_root/themes", 0777;
    if ($bitmap) {
        open(THEME, ">$rockbox_root/themes/rockbox_default_icons.cfg");
        print THEME <<STOP
# this config file was auto-generated to make it
# easy to reset the icons back to default
iconset: -
# taken from apps/gui/icon.c
viewers iconset: /$rockbox_root/icons/viewers.bmp
remote iconset: -
# taken from apps/gui/icon.c
remote viewers iconset: /$rockbox_root/icons/remote_viewers.bmp

STOP
;
        close(THEME);
    }
    
    mkdir "$rockbox_root/codepages", 0777;

    if($bitmap) {
        system("$ROOT/tools/codepages");
    }
    else {
        system("$ROOT/tools/codepages -m");
    }

    glob_move('*.cp', "$rockbox_root/codepages/");

    if($bitmap) {
        mkdir "$rockbox_root/codecs", 0777;
        if($depth > 1) {
            mkdir "$rockbox_root/backdrops", 0777;
        }

        find(find_copyfile(qr/.*\.codec/, abs_path("$rockbox_root/codecs/")), 'apps/codecs');

        # remove directory again if no codec was copied
        rmdir("$rockbox_root/codecs");
    }

    find(find_copyfile(qr/\.(rock|ovl)/, abs_path("$rockbox_root/rocks/")), 'apps/plugins');

    open VIEWERS, "$ROOT/apps/plugins/viewers.config" or
        die "can't open viewers.config";
    my @viewers = <VIEWERS>;
    close VIEWERS;

    open VIEWERS, ">$rockbox_root/viewers.config" or
        die "can't create $rockbox_root/viewers.config";

    foreach my $line (@viewers) {
        if ($line =~ /([^,]*),([^,]*),/) {
            my ($ext, $plugin)=($1, $2);
            my $r = "${plugin}.rock";
            my $oname;

            my $dir = $r;
            my $name;

            # strip off the last slash and file name part
            $dir =~ s/(.*)\/(.*)/$1/;
            # store the file name part
            $name = $2;

            # get .ovl name (file part only)
            $oname = $name;
            $oname =~ s/\.rock$/.ovl/;

            # print STDERR "$ext $plugin $dir $name $r\n";

            if(-e "$rockbox_root/rocks/$name") {
                if($dir ne "rocks") {
                    # target is not 'rocks' but the plugins are always in that
                    # dir at first!
                    move("$rockbox_root/rocks/$name", "$rockbox_root/rocks/$r");
                }
                print VIEWERS $line;
            }
            elsif(-e "$rockbox_root/rocks/$r") {
                # in case the same plugin works for multiple extensions, it
                # was already moved to the viewers dir
                print VIEWERS $line;
            }

            if(-e "$rockbox_root/rocks/$oname") {
                # if there's an "overlay" file for the .rock, move that as
                # well
                move("$rockbox_root/rocks/$oname", "$rockbox_root/rocks/$dir");
            }
        }
    }
    close VIEWERS;

    open CATEGORIES, "$ROOT/apps/plugins/CATEGORIES" or
        die "can't open CATEGORIES";
    my @rock_targetdirs = <CATEGORIES>;
    close CATEGORIES;
    foreach my $line (@rock_targetdirs) {
        if ($line =~ /([^,]*),(.*)/) {
            my ($plugin, $dir)=($1, $2);
            move("$rockbox_root/rocks/${plugin}.rock", "$rockbox_root/rocks/$dir/${plugin}.rock");
        }
    }

    if ($bitmap) {
        mkdir "$rockbox_root/icons", 0777;
        copy("$viewer_bmpdir/viewers.${icon_w}x${icon_h}x$depth.bmp", "$rockbox_root/icons/viewers.bmp");
        if ($remote_depth) {
            copy("$viewer_bmpdir/remote_viewers.${remote_icon_w}x${remote_icon_h}x$remote_depth.bmp", "$rockbox_root/icons/remote_viewers.bmp");
        }
    }

    copy("$ROOT/apps/tagnavi.config", "$rockbox_root/");
    copy("$ROOT/apps/plugins/disktidy.config", "$rockbox_root/rocks/apps/");

    if($bitmap) {
        copy("$ROOT/apps/plugins/sokoban.levels", "$rockbox_root/rocks/games/sokoban.levels"); # sokoban levels
        copy("$ROOT/apps/plugins/snake2.levels", "$rockbox_root/rocks/games/snake2.levels"); # snake2 levels
    }

    if($image) {
        # image is blank when this is a simulator
        if( filesize("rockbox.ucl") > 1000 ) {
            copy("rockbox.ucl", "$rockbox_root/rockbox.ucl");  # UCL for flashing
        }
        if( filesize("rombox.ucl") > 1000) {
            copy("rombox.ucl", "$rockbox_root/rombox.ucl");  # UCL for flashing
        }
        
        # Check for rombox.target
        if ($image=~/(.*)\.(\w+)$/)
        {
            my $romfile = "rombox.$2";
            if (filesize($romfile) > 1000)
            {
                copy($romfile, "$rockbox_root/$romfile");
            }
        }
    }

    mkdir "$rockbox_root/docs", 0777;
    for(("COPYING",
         "LICENSES",
         "KNOWN_ISSUES"
        )) {
        copy("$ROOT/docs/$_", "$rockbox_root/docs/$_.txt");
    }
    if ($fonts) {
        copy("$ROOT/docs/profontdoc.txt", "$rockbox_root/docs/profontdoc.txt");
    }
    for(("sample.colours",
         "sample.icons"
        )) {
        copy("$ROOT/docs/$_", "$rockbox_root/docs/$_");
    }

    # Now do the WPS dance
    if(-d "$ROOT/wps") {
	my $wps_build_cmd="perl $ROOT/wps/wpsbuild.pl ";
	$wps_build_cmd=$wps_build_cmd."-v " if $verbose;
	$wps_build_cmd=$wps_build_cmd." --rockroot=$rockbox_root -r $ROOT $ROOT/wps/WPSLIST $target";
	print "wpsbuild: $wps_build_cmd\n" if $verbose;
        system("$wps_build_cmd");
	print "wps_build_cmd: done\n" if $verbose;
    }
    else {
        print STDERR "No wps module present, can't do the WPS magic!\n";
    }

    # and the info file
    copy("rockbox-info.txt", "$rockbox_root/rockbox-info.txt");

    # copy the already built lng files
    glob_copy('apps/lang/*lng', "$rockbox_root/langs/");

}

my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) =
 localtime(time);

$mon+=1;
$year+=1900;

#$date=sprintf("%04d%02d%02d", $year,$mon, $mday);
#$shortdate=sprintf("%02d%02d%02d", $year%100,$mon, $mday);

# made once for all targets
sub runone {
    my ($target, $fonts)=@_;

    # build a full install .rockbox ($rockbox_root) directory
    buildzip($target, $fonts);

    unlink($output);

    if($fonts == 1) {
        # Don't include image file in fonts-only package
        undef $target;
    }
    if($target && ($target !~ /(mod|ajz|wma)\z/i)) {
        # On some targets, the image goes into .rockbox.
        copy("$target", "$rockbox_root/$target");
        undef $target;
    }

    if($verbose) {
        print "$ziptool $output $rockbox_root $target >/dev/null\n";
    }

    if($sim) {
        system("cp -r $rockbox_root archos/ >/dev/null");
    }
    else {
        system("$ziptool $output $rockbox_root $target >/dev/null");
    }

    # remove the .rockbox afterwards
    rmtree("$rockbox_root");
};

if(!$exe) {
    # not specified, guess!
    if($target =~ /(recorder|ondio)/i) {
        $exe = "ajbrec.ajz";
    }
    elsif($target =~ /iriver/i) {
        $exe = "rockbox.iriver";
    }
    else {
        $exe = "archos.mod";
    }
}
elsif($exe =~ /rockboxui/) {
    # simulator, exclude the exe file
    $exe = "";
}

runone($exe, $incfonts);


