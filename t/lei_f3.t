#!perl -w
# Copyright (C) all contributors <meta@public-inbox.org>
# License: AGPL-3.0+ <https://www.gnu.org/licenses/agpl-3.0.txt>
use v5.12;
use Test::More;
eval { require PublicInbox::LeiF3 };
if (my $err = $@) {
	plan skip_all => "no C compiler $err";
}
eval { PublicInbox::LeiF3::build(); };
if (my $err = $@) {
	my $pkg_config = $ENV{PKG_CONFIG} // 'pkg-config';
	like($err, qr/$pkg_config.*failed/, 'build failed');
} else {
	ok(-x "$ENV{PERL_INLINE_DIRECTORY}/f3/leifs.fuse",
		'built executable');
}

done_testing;
