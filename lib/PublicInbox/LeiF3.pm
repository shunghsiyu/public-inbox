# Copyright (C) all contributors <meta@public-inbox.org>
# License: AGPL-3.0+ <https://www.gnu.org/licenses/agpl-3.0.txt>

# Just-ahead-of-time builder for the lib/PublicInbox/f3.h FUSE3 shim.
# I never want users to be without source code for repairs, so this
# aims to replicate the feel of a scripting language using C11.
# This does NOT use Inline::C and the resulting executable is not
# linked to Perl in any way.
package PublicInbox::LeiF3;
use v5.12;
use Time::HiRes qw(stat);
use PublicInbox::Spawn;
my $dir = ($ENV{PERL_INLINE_DIRECTORY} //
	die('BUG: PERL_INLINE_DIRECTORY unset')) . '/f3';
my $F3_NS = 'lei';
my $bin = "$dir/${F3_NS}.fuse";
my ($srcpfx) = (__FILE__ =~ m!\A(.+/)[^/]+\z!);
my @srcs = map { $srcpfx.$_ } qw(f3.h);
my $xflags = ($ENV{CFLAGS} // '-Wall -ggdb3 -O0') . ' ' .
	($ENV{LDFLAGS} // '-Wl,-O1 -Wl,--compress-debug-sections=zlib') .
	qq{ -DF3_NS='"$F3_NS"'};

sub xflags_chg () {
	open my $fh, '<', "$dir/XFLAGS" or return 1;
	chomp(my $prev = <$fh>);
	$prev ne $xflags;
}

sub build () {
	if (!-d $dir) {
		my $err;
		mkdir($dir) or $err = $!;
		die "mkdir($dir): $err" if !-d $dir;
	}
	use autodie;
	require File::Temp;
	require Config;
	my ($prog) = ($bin =~ m!/([^/]+)\z!);
	my $pkg_config = $ENV{PKG_CONFIG} // 'pkg-config';
	my $tmp = File::Temp->newdir(DIR => $dir) // die "newdir: $!";
	my $src = "$tmp/$prog.c";
	open my $csrc, '>', $src;
	for (@srcs) {
		say $csrc qq(# line 1 "$_");
		open my $fh, '<', $_;
		local $/;
		print $csrc readline($fh);
	}
	close $csrc;
	my $cmd = "$pkg_config --libs --cflags fuse3 liburcu-cds liburcu-bp";
	chomp(my $fl = `$cmd`);
	die "$cmd failed: \$?=$?" if $?;
	my $cc = $ENV{CC} // $Config::Config{cc};
	$cmd = "$cc $src $fl $xflags -o $tmp/$prog";
	system($cmd) and die "$cmd failed: \$?=$?";
	my $cf = "$tmp/XFLAGS";
	open my $fh, '>', $cf;
	say $fh $xflags;
	close $fh;
	# not quite atomic, but close enough :P
	rename("$tmp/$_", "$dir/$_") for ($prog, 'XFLAGS');
}

sub start (@) {
	my $ctime = 0;
	my @bin = stat($bin);
	for ((@bin ? @srcs : ())) {
		my @st = stat($_) or die "stat $_: $!";
		$ctime = $st[10] if $st[10] > $ctime;
	}
	build() if !@bin || (@bin && $ctime > $bin[10]) || xflags_chg();
	my @cmd;
	if (my $v = $ENV{VALGRIND}) {
		$v = 'valgrind -v' if $v eq '1';
		@cmd = split(/\s+/, $v);
	}
	push @cmd, $bin, '-o', "subtype=$F3_NS,default_permissions", @_;
	my $prog = $cmd[0];
	$cmd[0] =~ s!\A.*?/([^/]+)\z!$1!;
	exec { $prog } @cmd;
}

1;
