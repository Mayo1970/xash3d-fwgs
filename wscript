#! /usr/bin/env python
# encoding: utf-8
# a1batross, mittorn, 2018

from waflib import Build, Configure, Context, Logs, TaskGen
from waflib.Tools import waf_unit_test, c_tests
import clang_format
import sys
import os

# we need version 17 minimum
clang_format.CLANG_FORMAT_MIN_MAJOR = 17

VERSION = '0.99'
APPNAME = 'xash3d-fwgs'
top = '.'
default_prefix = '/' # Waf uses it to set default prefix

Context.Context.line_just = 55 # should fit for everything on 80x26

c_tests.LARGE_FRAGMENT='''#include <unistd.h>
int check[sizeof(off_t) >= 8 ? 1 : -1]; int main(void) { return 0; }'''

@TaskGen.feature('cshlib', 'cxxshlib', 'fcshlib')
@TaskGen.before_method('apply_implib')
def remove_implib_install(self):
	if not getattr(self, 'install_path_implib', None):
		self.install_path_implib = None

@Configure.conf
def get_taskgen_count(self):
	# a build-wide (not per-path) taskgen counter, passed as idx= to force
	# a unique output object filename -- needed by hlsdk-portable/dlls and
	# hlsdk-portable/cl_dll, which both compile some of the same weapon
	# .cpp files (server and client builds of the same source) from two
	# different taskgen paths; without a shared global counter their
	# default per-path idx values can collide and waf silently reuses one
	# taskgen's compiled object for the other, dropping symbols the other
	# build actually needs (see AGENTS.md goal 10 notes).
	try: idx = self.tg_idx_count
	except AttributeError: idx = 0
	return idx

@TaskGen.feature('cprogram', 'cxxprogram')
@TaskGen.before_method('apply_flags_msvc')
def apply_subsystem_msvc(self):
	if getattr(self, 'subsystem', None):
		return # have custom subsystem

	if 'test' in self.features:
		self.subsystem = self.env.CONSOLE_SUBSYSTEM

class Subproject:
	def __init__(self, name, fnFilter = None):
		self.name = name
		self.fnFilter = fnFilter

	def is_exists(self, ctx):
		return ctx.path.find_node(self.name + '/wscript')

	def is_enabled(self, ctx):
		if not self.is_exists(ctx):
			return False

		if self.fnFilter:
			return self.fnFilter(ctx)

		return True

class RefDll:
	def __init__(self, name, default, key = None):
		self.name = name
		self.default = default
		self.dest = key if key else name.upper()

	def register_option(self, opt):
		kw = dict()
		if self.default:
			act = 'disable'
			kw['action'] = 'store_false'
		else:
			act = 'enable'
			kw['action'] = 'store_true'

		key = '--%s-%s' % (act, self.name)

		kw['dest'] = self.dest
		kw['default'] = self.default
		kw['help'] = '%s %s renderer [default: %%(default)s]' % (act, self.name)

		opt.add_option(key, **kw)

	def register_env(self, env, opts, force):
		env[self.dest] = force or opts.__dict__[self.dest]

	def register_define(self, conf):
		conf.define_cond('XASH_REF_%s_ENABLED' % self.dest, conf.env[self.dest])

SUBDIRS = [
	# always configured and built
	Subproject('public'),
	Subproject('filesystem'),
	Subproject('3rdparty/library_suffix'),

	# disable only by engine feature, makes no sense to even parse subprojects in dedicated mode
	Subproject('3rdparty/ps3gl',        lambda x: x.env.CLIENT and x.env.DEST_OS == 'ps3'),
	Subproject('ref/common',            lambda x: x.env.CLIENT),
	Subproject('ref/gl',                lambda x: x.env.CLIENT and x.env.GL),
	Subproject('ref/soft',              lambda x: x.env.CLIENT and x.env.SOFT),
	Subproject('3rdparty/bzip2',        lambda x: x.env.CLIENT and not x.env.HAVE_SYSTEM_BZ2),
	Subproject('3rdparty/opus',         lambda x: x.env.CLIENT and not x.env.HAVE_SYSTEM_OPUS),
	Subproject('3rdparty/opusfile',     lambda x: x.env.CLIENT and not x.env.HAVE_SYSTEM_OPUSFILE),
	# Same whole-tree swap as the two 'client' rows below, for the same reason.
	# Counter-Strike's menu is Velaron's mainui_cpp fork, not FWGS's: it is what
	# implements IGameMenuExports / "GameMenuExports001", which cs16-client's
	# cdll_int.cpp pulls through the engine's MenuFactory native object to drive
	# the buy, team and class menus. Exactly one of these supplies the waf name
	# 'menu' that xshlib.py links.
	Subproject('3rdparty/mainui',       lambda x: x.env.CLIENT and x.env.PS3_GAME != 'cstrike'),
	Subproject('3rdparty/mainui_cs',    lambda x: x.env.CLIENT and x.env.PS3_GAME == 'cstrike'),
	Subproject('3rdparty/MultiEmulator',lambda x: x.env.CLIENT),
	Subproject('hlsdk-portable/game_shared'),
	# Exactly one of these supplies the waf name 'server'. Counter-Strike's
	# server is ReGameDLL_CS, a separate SDK with its own game_shared/
	# pm_shared/public/engine snapshots, so the cstrike flavor swaps the whole
	# server tree the same way it swaps the client and the menu below.
	Subproject('hlsdk-portable/dlls',   lambda x: x.env.PS3_GAME != 'cstrike'),
	Subproject('regamedll/dlls',        lambda x: x.env.PS3_GAME == 'cstrike'),
	# Exactly one of these supplies the waf name 'client' that xshlib.py links
	# in. Counter-Strike is a separate SDK rather than an hlsdk-portable
	# variant, so the cstrike flavor swaps the whole client tree instead of
	# gating hlsdk-portable's with a define. PS3_GAME is unset everywhere but
	# a PS3 flavor build, so every other target keeps the hlsdk client.
	Subproject('hlsdk-portable/cl_dll',  lambda x: x.env.CLIENT and x.env.PS3_GAME != 'cstrike'),
	Subproject('cs16-client/cl_dll',     lambda x: x.env.CLIENT and x.env.PS3_GAME == 'cstrike'),
	Subproject('engine'), # keep latest for static linking
]

REFDLLS = [
	RefDll('soft', True),
	RefDll('gl', True),
]

# Per-flavor PS3 package identity, keyed by --gamedir. Each mod ships as its
# own installable PKG with its own XMB entry, since PSL1GHT has no dlopen and
# so no runtime mod switching is possible -- the flavor is a build-time choice.
# Only the XMB/PKG identity below is per-flavor: every flavor resolves its
# filesystem root to the same shared /dev_hdd0/data/xash3dfwgs (see
# FS_DetermineRootDirectory), so valve/ isn't duplicated once per install and
# each mod gamedir falls back to it via its liblist.gam fallback_dir key.
# TITLE_ID must be exactly 9 characters -- LV2 rejects anything else.
# The fourth field is the flavor's build directory: the stock build stays in
# build/, every mod gets its own build_* so the two never share objects (they
# compile the same sources with different -D flags, so a shared build dir
# would hand one flavor the other's .o files).
PS3_FLAVORS = {
	'valve':   ('Xash3D FWGS',                   'XASH10000', 'icons/hl1/ICON0.PNG', 'build'),
	'bshift':  ('Xash3D FWGS (Blue Shift)',      'XASHBS000', 'icons/bs/ICON0.PNG',  'build_bs'),
	'gearbox': ('Xash3D FWGS (Opposing Force)',  'XASHOF000', 'icons/of/ICON0.PNG',  'build_opfor'),
	'ricochet':('Xash3D FWGS (Ricochet)',        'XASHRC000', 'icons/ricochet/ICON0.PNG', 'build_ricochet'),
	'cstrike': ('Xash3D FWGS (Counter-Strike)',  'XASHCS000', 'icons/cs/ICON0.PNG',  'build_cs'),
}

# Game-code define per flavor. One vendored hlsdk-portable tree serves every
# flavor -- there is no per-mod source tree -- so each mod's divergence from
# base HL1 lives inline behind its own #ifdef and the valve build must stay
# semantically identical to upstream master. BSHIFT covers the FWGS
# hlsdk-portable `bshift` branch: item_armorvest/item_helmet, monster_rosenberg,
# trigger_playerfreeze, env_warpball, monster_generic head tracking and the
# blue HUD. Entity symbols these add go in dlls/exports_<gamedir>.txt, since
# exports.txt is emitted as extern declarations and the valve link would fail
# on names its own #ifdef'd-out sources never define.
# OPFOR covers the FWGS hlsdk-portable `opforfixed` branch (Opposing Force).
# Its gamedir -- and so its flavor key here -- is `gearbox`, the directory the
# mod actually ships under; the define is named for the game, not the folder.
# Unlike BSHIFT, most of Opposing Force is new source under dlls/gearbox/ and
# cl_dll/gearbox/ rather than inline #ifdefs: those files are excluded from
# every other flavor's glob in hlsdk-portable/{dlls,cl_dll}/wscript, since an
# #ifdef inside them would still drag 57 files of gearbox entities into the
# valve link.
# RICOCHET has no game code yet: the flavor currently builds base HL1 sources
# under the ricochet gamedir, which is enough to boot to the menu but not to
# play. Ricochet is a total conversion (one disc weapon, no monsters) and its
# source is Valve's own ricochet/ tree in ValveSoftware/halflife rather than an
# hlsdk-portable branch, so landing it means following the shape of FWGS's dmc
# branch -- mod code under {dlls,cl_dll}/ricochet/, HL-only sources excluded
# from this flavor's glob, and a REPLACED rather than extended exports list.
# CSTRIKE has deliberately NO entry here. Counter-Strike is not a variant of the
# hlsdk-portable tree at all: its client is the separate cs16-client SDK and its
# server is ReGameDLL_CS, so the flavor selects a whole different source tree
# rather than gating this one, and a define here would gate nothing. That swap
# has landed for the client half -- see the two 'client' rows in SUBDIRS, which
# pick cs16-client/cl_dll for this flavor and hlsdk-portable/cl_dll for every
# other -- and for the menu, via the matching 'menu' pair that picks
# 3rdparty/mainui_cs (Velaron's mainui_cpp fork, which implements
# GameMenuExports001 and so the buy/team/class menus) over 3rdparty/mainui.
# The server is still base HL1, so local play is HL1 rules, not CS.
PS3_GAME_DEFINES = {
	'bshift':   ['BSHIFT'],
	'gearbox':  ['OPFOR'],
	'ricochet': ['RICOCHET'],
}

# Build directory, derived from --gamedir so the stock and mod trees never
# clobber each other. waf reads `out` when it loads this module, long before
# options are parsed, so this has to read argv directly; an explicit -o/--out
# still wins, since waf applies that afterwards.
out = PS3_FLAVORS['valve'][3]
for _i, _arg in enumerate(sys.argv[1:]):
	_game = None
	if _arg.startswith('--gamedir='):
		_game = _arg.split('=', 1)[1]
	elif _arg == '--gamedir' and _i + 2 < len(sys.argv):
		_game = sys.argv[_i + 2]
	if _game in PS3_FLAVORS:
		out = PS3_FLAVORS[_game][3]

def options(opt):
	opt.load('reconfigure compiler_optimizations xshlib xcompile compiler_cxx compiler_c sdl2 clang_compilation_database strip_on_install waf_unit_test msvs subproject ninja')

	grp = opt.add_option_group('Common options')

	grp.add_option('-d', '--dedicated', action = 'store_true', dest = 'DEDICATED', default = False,
		help = 'only build Xash Dedicated Server [default: %(default)s]')

	grp.add_option('--enable-dedicated', action = 'store_true', dest = 'ENABLE_DEDICATED', default = False,
		help = 'enable building Xash Dedicated Server alongside client [default: %(default)s]')

	grp.add_option('--enable-tui', action = 'store_true', dest = 'ENABLE_TUI', default = False,
		help = 'enable TUI main menu [default: %(default)s]')

	grp.add_option('--gamedir', action = 'store', dest = 'GAMEDIR', default = 'valve',
		help = 'engine default (base) game directory [default: %(default)s]')

	grp.add_option('-8', '--64bits', action = 'store_true', dest = 'ALLOW64', default = False,
		help = 'allow targetting 64-bit engine(Linux/Windows only) [default: %(default)s]')

	grp.add_option('-4', '--32bits', action = 'store_true', dest = 'FORCE32', default = False,
		help = 'force targetting 32-bit engine, usually unneeded [default: %(default)s]')

	grp.add_option('-P', '--enable-packaging', action = 'store_true', dest = 'PACKAGING', default = False,
		help = 'respect prefix option, useful for packaging for various operating systems [default: %(default)s]')

	grp.add_option('--enable-bundled-deps', action = 'store_true', dest = 'BUILD_BUNDLED_DEPS', default = False,
		help = 'prefer to build bundled dependencies (like opus) instead of relying on system provided')

	grp.add_option('--enable-hl25-extended-structs', action = 'store_true', dest = 'SUPPORT_HL25_EXTENDED_STRUCTS', default = False,
		help = 'build engine and renderers with HL25 extended structs compatibility (might be required for some mods) [default: %(default)s]')

	grp.add_option('--low-memory-mode', action = 'store', dest = 'LOW_MEMORY', default = 0, type = int,
		help = 'enable low memory mode (only for devices have <128 ram)')

	grp.add_option('--disable-werror', action = 'store_true', dest = 'DISABLE_WERROR', default = False,
		help = 'disable compilation abort on warning')

	grp.add_option('--enable-tests', action = 'store_true', dest = 'TESTS', default = False,
		help = 'enable building standalone tests (does not enable engine tests!) [default: %(default)s]')

	grp.add_option('--disable-rpath', action = 'store_false', dest = 'ENABLE_RPATH', default = True,
		help = 'disables rpath, duh!')

	# a1ba: special option for me
	grp.add_option('--enable-msvcdeps', action='store_true', dest='MSVCDEPS', default=False, help='')
	grp.add_option('--enable-wafcache', action='store_true', dest='WAFCACHE', default=False, help='')

	grp = opt.add_option_group('Renderers options')

	grp.add_option('--enable-all-renderers', action='store_true', dest='ALL_RENDERERS', default=False,
		help = 'enable all renderers supported by Xash3D FWGS [default: %(default)s]')

	for dll in REFDLLS:
		dll.register_option(grp)

	grp = opt.add_option_group('Utilities options')

	grp.add_option('--enable-utils', action = 'store_true', dest = 'ENABLE_UTILS', default = False,
		help = 'enable building various development utilities [default: %(default)s]')

	grp.add_option('--enable-xar', action = 'store_true', dest = 'ENABLE_XAR', default = False,
		help = 'enable building Xash ARchiver (experimental) [default: %(default)s]')

	grp.add_option('--enable-fuzzer', action = 'store_true', dest = 'ENABLE_FUZZER', default = False,
		help = 'enable building libFuzzer runner [default: %(default)s]' )

	for i in SUBDIRS:
		if not i.is_exists(opt):
			continue

		opt.add_subproject(i.name)

def configure(conf):
	conf.load('fwgslib reconfigure compiler_optimizations')
	if conf.options.ALLOW64:
		conf.env.MSVC_TARGETS = ['x64']
	elif sys.maxsize > 2 ** 32 and not conf.options.MSVC_WINE:
		conf.env.MSVC_TARGETS = ['amd64_x86', 'x86']
	else:
		conf.env.MSVC_TARGETS = ['x86']

	# Load compilers early
	# NOTE: xcompile must load before xshlib -- on cross targets (e.g. PS3)
	# xcompile.configure() sets conf.environ['LD']/['OBJCOPY'] to the cross
	# binutils, which xshlib.configure()'s conf.find_program('ld'/'objcopy')
	# (used by --static-linking) needs to already be in place, or it silently
	# falls back to the host's ld/objcopy.
	conf.load('xcompile xshlib compiler_c compiler_cxx')

	if not conf.options.WAFCACHE:
		conf.load('gccdeps')

		if conf.options.MSVCDEPS:
			conf.load('msvcdeps')

	conf.env.WAFCACHE = conf.options.WAFCACHE

	if conf.options.NSWITCH:
		conf.load('nswitch')

	if conf.options.PSVITA:
		conf.load('psvita')

	if conf.options.PS3:
		conf.load('ps3')

	# HACKHACK: override msvc DEST_CPU value by something that we understand
	if conf.env.DEST_CPU == 'amd64':
		conf.env.DEST_CPU = 'x86_64'

	if conf.env.COMPILER_CC == 'msvc':
		conf.load('msvc_pdb')

	conf.load('msvs subproject clang_compilation_database strip_on_install waf_unit_test enforce_pic force_32bit ninja clang_format')

	conf.env.MSVC_SUBSYSTEM = 'WINDOWS'
	conf.env.CONSOLE_SUBSYSTEM = 'CONSOLE'

	# Windows XP compatibility
	if conf.env.MSVC_TARGETS[0] == 'amd64_x86' or conf.env.MSVC_TARGETS[0] == 'x86':
		conf.env.MSVC_SUBSYSTEM += ',5.01'
		conf.env.CONSOLE_SUBSYSTEM += ',5.01'

	# Set default options for some platforms
	if conf.env.DEST_OS == 'android':
		conf.options.NANOGL           = True
		conf.options.GLWES            = False # deprecated
		conf.options.GL4ES            = True
		conf.options.GLES3COMPAT      = True
		conf.options.GL               = False
	elif conf.env.IOS:
		conf.options.NANOGL           = True
		conf.options.GLWES            = False # deprecated
		conf.options.GL4ES            = False # doesn't compile on ios yet
		conf.options.GLES3COMPAT      = True
		conf.options.GL               = False
	elif conf.env.MAGX:
		conf.options.SDL12            = True
		conf.options.GL               = False
		conf.options.LOW_MEMORY       = 1
		enforce_pic = False
	elif conf.env.DEST_OS == 'emscripten':
		conf.options.BUILD_BUNDLED_DEPS = True
		conf.options.GLES3COMPAT      = True
		conf.options.GL               = False
	elif conf.env.DEST_OS == 'ps3':
		# ref_gl runs on 3rdparty/ps3gl, a GL 1.1 fixed-function subset over the
		# RSX. PSL1GHT still has no *runtime* shader compiler, but ps3gl doesn't
		# need one: its Cg fragment/vertex programs are compiled offline with
		# cgcomp and checked in. Same shape as PSVita/vitaGL -- see ref/gl/wscript.
		# ref_soft stays enabled as the `-ref soft` fallback.
		conf.options.GL               = True
		# off by default: an unauthenticated UDP sender to a hardcoded dev IP
		# has no business in a release build. --enable-ps3-udp-log opts in
		# for dev/debug builds (see engine/platform/ps3/sys_ps3.c).
		conf.define_cond('XASH_PS3_UDP_LOG', conf.options.PS3_UDP_LOG)

	# psvita needs -fPIC set manually and static builds are incompatible with -fPIC
	enforce_pic = conf.env.DEST_OS != 'psvita' and not conf.env.STATIC_LINKING
	conf.check_pic(enforce_pic)

	# NOTE: We restrict 64-bit builds ONLY for Win/Linux running on Intel architecture
	# Because compatibility with original GoldSrc
	# NOTE: Since modern OSX (since Catalina) don't support 32-bit applications, there is no point
	# to restrict them to 32-bit engine, despite GoldSrc is still officially supported.
	# There is now `-4` (or `--32bits`) configure flag for those
	# who want to specifically build engine for 32-bit
	if conf.env.DEST_OS in ['win32', 'linux'] and conf.env.DEST_CPU == 'x86_64':
		force_32bit = not conf.options.ALLOW64
	else:
		force_32bit = conf.options.FORCE32

	if force_32bit:
		conf.force_32bit()

	cflags, linkflags = conf.get_optimization_flags()
	cxxflags = list(cflags) # optimization flags are common between C and C++ but we need a copy

	# on the Switch, allow undefined symbols by default, which is needed for libsolder to work
	# we'll specifically disallow them for the engine executable
	# additionally, shared libs are linked without standard libs, we'll add those back in the engine wscript
	if conf.env.DEST_OS == 'nswitch':
		linkflags.remove('-Wl,--no-undefined')
		conf.env.append_unique('LINKFLAGS_cshlib', ['-nostdlib', '-nostartfiles'])
		conf.env.append_unique('LINKFLAGS_cxxshlib', ['-nostdlib', '-nostartfiles'])
	# same on the vita
	elif conf.env.DEST_OS == 'psvita':
		conf.env.append_unique('CFLAGS_cshlib', ['-fPIC'])
		conf.env.append_unique('CXXFLAGS_cxxshlib', ['-fPIC', '-fno-use-cxa-atexit'])
		conf.env.append_unique('LINKFLAGS_cshlib', ['-nostdlib', '-Wl,--unresolved-symbols=ignore-all'])
		conf.env.append_unique('LINKFLAGS_cxxshlib', ['-nostdlib', '-Wl,--unresolved-symbols=ignore-all'])
	# check if we need to use irix linkflags
	elif conf.env.DEST_OS == 'irix' and conf.env.COMPILER_CC == 'gcc':
		linkflags.remove('-Wl,--no-undefined')
		linkflags.append('-Wl,-u,gl_INTERPRET_END')
		# check if we're in a sgug environment
		if 'sgug' in os.environ['LD_LIBRARYN32_PATH']:
			linkflags.append('-lc')
	elif conf.env.DEST_OS == 'darwin':
		try:
			linkflags.remove('-Wl,--no-undefined')
		except:
			pass
		linkflags.append('-Wl,-undefined,error')
	elif conf.env.SAILFISH:
		conf.define('XASH_SAILFISH', 1)

	conf.check_cc(cflags=cflags, linkflags=linkflags, msg='Checking for required C flags')
	conf.check_cxx(cxxflags=cxxflags, linkflags=linkflags, msg='Checking for required C++ flags')

	conf.env.append_unique('CFLAGS', cflags)
	conf.env.append_unique('CXXFLAGS', cxxflags)
	conf.env.append_unique('LINKFLAGS', linkflags)

	if conf.env.COMPILER_CC == 'msvc':
		opt_cflags = ['/we4013'] # -Werror=implicit-function-declaration
		conf.env.CFLAGS_werror = conf.filter_cflags(opt_cflags, cflags)
	else:
		opt_flags = [
			# '-Wall', '-Wextra', '-Wpedantic',
			'-fdiagnostics-color=always',

			# stable diagnostics, forced to error, sorted
			'-Werror=alloc-size',
			'-Werror=bool-compare',
			'-Werror=bool-operation',
			# '-Werror=cast-align=strict',
			'-Werror=duplicated-cond',
			'-Werror=format=2',
			'-Werror=free-nonheap-object',
			'-Werror=implicit-fallthrough=2',
			'-Werror=logical-op',
			'-Werror=nonnull',
			'-Werror=packed',
			'-Werror=packed-not-aligned',
			'-Werror=parentheses',
			'-Werror=return-type',
			'-Werror=sequence-point',
			'-Werror=sizeof-pointer-memaccess',
			'-Werror=sizeof-array-div',
			'-Werror=sizeof-pointer-div',
			'-Werror=strict-aliasing',
			'-Werror=string-compare',
			'-Werror=tautological-compare',
			'-Werror=use-after-free=3',
			'-Werror=vla',
			'-Werror=write-strings',

			# unstable diagnostics, may cause false positives
			'-Walloc-zero',
			'-Winit-self',
			'-Wmisleading-indentation',
			'-Wmismatched-dealloc',
			'-Wstringop-overflow',
			'-Wuninitialized',
			'-Wno-error=format-nonliteral',

			# disabled, flood
			# '-Wdouble-promotion',

			'-Wunused-function',
			'-Wunused-variable',
			'-Wunused-but-set-variable',
		]

		if conf.env.COMPILER_CC == 'clang':
			opt_flags += [
				'-Werror=unsequenced', # clang's version of -Werror=sequence-point
			]

		opt_cflags = [
			# disabled, as we're targetting C99 at least
			# '-Werror=declaration-after-statement',
			'-Werror=enum-conversion',
			'-Wno-error=enum-float-conversion', # need this for cvars
			'-Werror=implicit-int',
			'-Werror=implicit-function-declaration',
			'-Werror=incompatible-pointer-types',
			'-Werror=int-conversion',
			'-Werror=jump-misses-init',
			'-Werror=old-style-declaration',
			'-Werror=old-style-definition',
			'-Werror=strict-prototypes',
			'-fnonconst-initializers', # owcc
			'-Wmissing-prototypes', # not an error yet
		]

		opt_cxxflags = [] # TODO:

		if conf.options.DISABLE_WERROR:
			opt_flags = []
			opt_cflags = ['-Werror=implicit-function-declaration']
			opt_cxxflags = []

		conf.env.CFLAGS_werror = conf.filter_cflags(opt_flags + opt_cflags, cflags)
		conf.env.CXXFLAGS_werror = conf.filter_cxxflags(opt_flags + opt_cxxflags, cxxflags)

		# -Werror=format=2 turns on format-nonliteral as an error, which upstream
		# hlsdk-portable trips in cl_dll/text_message.cpp (safe_snprintf is a plain
		# snprintf off Win32 and is called with a runtime format string). The
		# demotion is already in opt_flags, but filter_cxxflags does not reliably
		# keep it under ppu-g++ 7.2 -- it survives the C filter and not the C++ one,
		# so a fresh configure builds the C engine and then fails the C++ game DLLs.
		# Re-append it wherever the promotion made it through, matching the intent
		# the flag list already states.
		for werror_flags in (conf.env.CFLAGS_werror, conf.env.CXXFLAGS_werror):
			if '-Werror=format=2' in werror_flags and '-Wno-error=format-nonliteral' not in werror_flags:
				werror_flags.append('-Wno-error=format-nonliteral')

	conf.env.TESTS         = conf.options.TESTS
	conf.env.ENABLE_UTILS  = conf.options.ENABLE_UTILS
	conf.env.ENABLE_XAR    = conf.options.ENABLE_XAR
	conf.env.ENABLE_FUZZER = conf.options.ENABLE_FUZZER

	if not conf.options.DEDICATED:
		conf.env.SERVER = conf.options.ENABLE_DEDICATED
		conf.env.CLIENT = True
		conf.env.LAUNCHER = conf.env.DEST_OS not in ['android', 'nswitch', 'psvita', 'ps3', 'dos', 'emscripten'] and not conf.env.IOS and not conf.env.MAGX and not conf.env.STATIC_LINKING
	else:
		conf.env.SERVER = True
		conf.env.CLIENT = False
		conf.env.LAUNCHER = False

	conf.env.TUI = conf.options.ENABLE_TUI

	conf.define_cond('SUPPORT_HL25_EXTENDED_STRUCTS', conf.options.SUPPORT_HL25_EXTENDED_STRUCTS)

	if conf.options.ENABLE_RPATH and conf.env.DEST_OS not in ['nswitch', 'psvita', 'ps3']:
		if conf.env.DEST_OS == 'openbsd':
			# OpenBSD requires -z origin to enable $ORIGIN expansion in RPATH
			conf.env.RPATH_ST = '-Wl,-z,origin,-rpath,%s'
			conf.env.DEFAULT_RPATH = '$ORIGIN'
		elif conf.env.DEST_OS == 'irix':
			conf.env.DEFAULT_RPATH = '/usr/lib32:/usr/sgug/lib32'
		elif conf.env.DEST_OS == 'darwin':
			conf.env.DEFAULT_RPATH = '@loader_path'
		else:
			conf.env.DEFAULT_RPATH = '$ORIGIN'

	setattr(conf, 'refdlls', REFDLLS)

	for refdll in REFDLLS:
		refdll.register_env(conf.env, conf.options, conf.options.ALL_RENDERERS)

	conf.env.GAMEDIR = conf.options.GAMEDIR

	if conf.env.DEST_OS == 'ps3':
		flavor = PS3_FLAVORS.get(conf.env.GAMEDIR)
		if not flavor:
			conf.fatal('no PS3 flavor defined for gamedir \'%s\' (known: %s)' % (
				conf.env.GAMEDIR, ', '.join(sorted(PS3_FLAVORS))))

		conf.env.PS3_TITLE, conf.env.PS3_APPID, conf.env.PS3_ICON0, ps3_out = flavor

		if len(set(f[3] for f in PS3_FLAVORS.values())) != len(PS3_FLAVORS):
			conf.fatal('two PS3 flavors share a build directory -- they compile '
				'the same sources with different -D flags and would swap objects')

		if len(conf.env.PS3_APPID) != 9:
			conf.fatal('PS3 TITLE_ID \'%s\' must be exactly 9 characters' % conf.env.PS3_APPID)

		if not conf.srcnode.find_node(conf.env.PS3_ICON0):
			conf.fatal('PS3 icon \'%s\' not found in source tree' % conf.env.PS3_ICON0)

		# XASH_GAMEDIR is the engine *basedir*, not the mod directory: host.c
		# hands it to InitStdio as basedir, and searchpath.c only adds the base
		# hierarchy when basedir differs from the gamefolder. Folding both into
		# XASH_GAMEDIR drops valve/ out of the search path entirely, which costs
		# you every shared asset -- localized menu strings included. So the mod
		# goes through -game instead, exactly like `xash -game bshift` on PC,
		# and the base stays valve. Every HL-family flavor shares that base; if
		# a non-HL-based game is ever added, promote this to a table field.
		conf.env.PS3_GAME = conf.env.GAMEDIR
		conf.env.GAMEDIR = 'valve'
		conf.define('XASH_PS3_GAME', conf.env.PS3_GAME)

		# consumed by hlsdk-portable/{dlls,cl_dll}/wscript
		conf.env.GAME_DEFINES = PS3_GAME_DEFINES.get(conf.env.PS3_GAME, [])

		conf.msg('PS3 flavor', '%s (%s, -game %s, %s/)' % (
			conf.env.PS3_TITLE, conf.env.PS3_APPID, conf.env.PS3_GAME, ps3_out))

	conf.define('XASH_GAMEDIR', conf.env.GAMEDIR)

	if conf.env.DEST_OS == 'nswitch':
		conf.check_cfg(package='solder', args='--cflags --libs', uselib_store='SOLDER')
		if conf.env.HAVE_SOLDER and conf.env.LIB_SOLDER and conf.options.BUILD_TYPE == 'debug':
			conf.env.LIB_SOLDER[0] += 'd' # load libsolderd in debug mode
		conf.check_cc(lib='m')
	elif conf.env.DEST_OS == 'psvita':
		conf.check_cc(lib='vrtld')
		conf.check_cc(lib='m')
	elif conf.env.DEST_OS == 'ps3':
		# no dlopen on PS3 -- skip the generic 'dl' check the else branch would do
		conf.check_cc(lib='m')
	elif conf.env.DEST_OS == 'android':
		# maybe there is some better check?
		if conf.find_program('termux-info', mandatory=False):
			conf.env.TERMUX = True
			conf.define('__TERMUX__', 1)

		conf.check_cc(lib='dl')
		conf.check_cc(lib='log')
		if not conf.options.ANDROID_OPTS:
			# if we're compiling on device itself
			conf.check_cc(lib='m')
		# otherwise LIB_M is defined by xcompile (as it might be libm_hard, depending on NDK configuration)
	elif conf.env.DEST_OS == 'win32':
		# Common Win32 libraries
		# Don't check them more than once, to save time
		# Usually, they are always available
		# but we need them in uselib
		a = [ 'user32', 'shell32', 'gdi32', 'advapi32', 'dbghelp', 'psapi', 'ws2_32', 'bcrypt' ]
		if conf.env.COMPILER_CC == 'msvc':
			for i in a:
				conf.start_msg('Checking for MSVC library')
				conf.check_lib_msvc(i)
				conf.end_msg(i)
		else:
			for i in a:
				conf.check_cc(lib = i)
	else:
		conf.check_cc(lib='dl', mandatory = False)
		conf.check_cc(lib='m')

	# hlsdk-portable/dlls and cl_dll fall back to external/openbsd/strlcpy.c
	# and strlcat.c when the target libc doesn't provide them -- probe for
	# real support instead of assuming, since PSL1GHT's libc is a partial
	# newlib build with known gaps elsewhere (see AGENTS.md section 6).
	conf.env.HAVE_STRLCPY = conf.check_cc(
		fragment='#include <string.h>\nint main(int argc, char **argv) { return strlcpy(argv[1], argv[2], 10); }',
		msg='Checking for strlcpy', mandatory=False)
	conf.env.HAVE_STRLCAT = conf.check_cc(
		fragment='#include <string.h>\nint main(int argc, char **argv) { return strlcat(argv[1], argv[2], 10); }',
		msg='Checking for strlcat', mandatory=False)

	# set _FILE_OFFSET_BITS=64 for filesystems with 64-bit inodes
	# must be set globally as it changes ABI
	if conf.env.DEST_OS == 'android' and conf.env.DEST_SIZEOF_VOID_P == 4:
		# Android in 32-bit mode don't have good enough large file support
		# with our native API level
		# https://android.googlesource.com/platform/bionic/+/HEAD/docs/32-bit-abi.md
		pass
	elif conf.env.DEST_OS == 'psvita':
		# PSVita don't have large file support at all
		pass
	else:
		# try to guess how to support large files
		conf.check_large_file(compiler = 'c', execute = False)

	# indicate if we are packaging for Linux/BSD
	if conf.options.PACKAGING:
		conf.env.PREFIX = conf.options.prefix
		if conf.env.SAILFISH:
			conf.env.SHAREDIR = '${PREFIX}/share/harbour-xash3d-fwgs/rodir'
		else:
			conf.env.SHAREDIR = '${PREFIX}/share/xash3d'
			conf.env.LIBDIR += '/xash3d'
	else:
		conf.env.SHAREDIR = conf.env.LIBDIR = conf.env.BINDIR = conf.env.PREFIX

	if not conf.options.BUILD_BUNDLED_DEPS:
		# there was a check for system libbacktrace but we can't be sure if it supports fileline or not
		# therefore, always build libbacktrace ourselves

		if conf.env.CLIENT:
			for i in ('ogg','opusfile','vorbis','vorbisfile'):
				if conf.check_cfg(package=i, uselib_store=i, args='--cflags --libs', mandatory=False):
					conf.env['HAVE_SYSTEM_%s' % i.upper()] = True

				if conf.env.HAVE_SYSTEM_OPUSFILE:
					frag='''#include <opusfile.h>
int main(int argc, char **argv) { return opus_tagcompare(argv[0], argv[1]); }'''

					conf.env.HAVE_SYSTEM_OPUSFILE = conf.check_cc(msg='Checking for libopusfile sanity', use='opusfile werror', fragment=frag, mandatory=False)

			# search for opus 1.4 only, it has fixes for custom modes
			# 1.5 breaks custom modes: https://github.com/xiph/opus/issues/374
			have_opus = conf.check_cfg(package='opus', uselib_store='opus', args=['opus = 1.4', '--cflags', '--libs'], mandatory=False)

			# 1.6.1 fixes them again
			if not have_opus:
				have_opus = conf.check_cfg(package='opus', uselib_store='opus', args=['opus >= 1.6.1', '--cflags', '--libs'], mandatory=False)

			if have_opus:
				# now try to link with export that only exists with CUSTOM_MODES defined
				frag='''#include <opus_custom.h>
int main(void) { return !opus_custom_encoder_init((OpusCustomEncoder *)1, (const OpusCustomMode *)1, 1); }'''

				conf.env.HAVE_SYSTEM_OPUS = conf.check_cc(msg='Checking if opus supports custom modes', defines='CUSTOM_MODES=1', use='opus werror', fragment=frag, mandatory=False)

			# search for bzip2
			BZIP2_CHECK='''#include <bzlib.h>
int main(void) { return (int)BZ2_bzlibVersion(); }'''

			conf.env.HAVE_SYSTEM_BZ2 = conf.check_cc(lib='bz2', fragment=BZIP2_CHECK, uselib_store='bzip2', mandatory=False)

	conf.define('XASH_LOW_MEMORY', conf.options.LOW_MEMORY)

	for i in SUBDIRS:
		if not i.is_enabled(conf):
			continue

		conf.add_subproject(i.name)

def build(bld):
	# enable progress bar mode if stdout is a terminal
	if not bld.progress_bar and sys.stdout.isatty():
		bld.progress_bar = 1

	if bld.env.WAFCACHE:
		bld.load('wafcache')

	# guard rails to not let install to root
	if bld.is_install and not bld.options.PACKAGING and not bld.options.destdir:
		bld.fatal('Set the install destination directory using --destdir option')

	# don't clean QtCreator files and reconfigure saved options
	bld.clean_files = bld.bldnode.ant_glob('**',
		excl='.qtc* *.user configuration.py .lock* *conf_check_*/** config.log %s/*' % Build.CACHE_DIR,
		quiet=True, generator=True)

	bld.load('xshlib')

	for i in SUBDIRS:
		if not i.is_enabled(bld):
			continue

		bld.add_subproject(i.name)

	if bld.env.TESTS:
		bld.add_post_fun(waf_unit_test.summary)
		bld.add_post_fun(waf_unit_test.set_exit_code)
