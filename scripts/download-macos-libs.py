#!/usr/bin/env python3
"""
Downloads required libraries for xemu builds on macOS from MacPorts repositories
"""
# Based on https://github.com/tpoechtrager/osxcross/blob/master/tools/osxcross-macports
# which is based on https://github.com/maci0/pmmacports
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import gzip
import glob
import re
import os.path
from tarfile import TarFile
import subprocess
import tarfile

MIRROR = 'https://packages.macports.org'

# FIXME: Inline macports key
# FIXME: Move packages to archive directory to track used vs unused
# FIXME: Support multiple mirrors

class LibInstaller:
	DARWIN_TARGET_X64="darwin_21" # macOS 11
	DARWIN_TARGET_ARM64="darwin_23" # macOS 14.x

	def __init__(self, arch):
		self._queue = []
		self._installed = [] # fetched by this run
		if arch == 'x86_64':
			self._darwin_target = self.DARWIN_TARGET_X64
		elif arch == 'arm64':
			self._darwin_target = self.DARWIN_TARGET_ARM64
		else:
			assert False, "Add arch"
		self._arch = arch

		self._extract_path = os.path.abspath(f'./macos-libs/{self._arch}')
		if not os.path.exists(self._extract_path):
			os.makedirs(self._extract_path)
		self._installed_path = os.path.join(self._extract_path, 'INSTALLED')
		self._pkgs_path = os.path.abspath(os.path.join(f'./macos-pkgs'))
		if not os.path.exists(self._pkgs_path):
			os.makedirs(self._pkgs_path)

	def list_pkgs(self, pkg_name, timeout=None):
		"""The mirror's builds of pkg_name for this target, as file names."""
		req = Request(f'{MIRROR}/{pkg_name}/', headers={'Accept-Encoding': 'gzip'})
		with urlopen(req, timeout=timeout) as response:
			pkg_list = response.read()
			if response.headers.get('Content-Encoding', '').strip().lower() in ('gzip', 'x-gzip'):
				pkg_list = gzip.decompress(pkg_list)
		pkg_list = pkg_list.decode('utf-8')
		return set(re.findall(pkg_name + r'[\w\.\-\_\+]*?\.(?:any_any|darwin_any|' + self._darwin_target + r')\.(?:noarch|' + self._arch + r')\.tbz2', pkg_list))

	def get_latest_pkg_filename_url(self, pkg_name):
		pkg_base_url = f'{MIRROR}/{pkg_name}'
		pkgs = self.list_pkgs(pkg_name)
		if len(pkgs) < 1:
			print(f'    [*] [ERROR] Unable to find version of {pkg_name} compatible with {self._darwin_target}.{self._arch}')
			exit(1)

		# The mirror lists files alphabetically, so the last match is the
		# lexicographically greatest, not the newest: SDL3-3.4.8 sorted after
		# SDL3-3.4.18 and the 3.4.8 build was vendored for months. Rank by the
		# numeric version, then the port revision; only when both tie does the
		# build with the fewest +variants win, so a newer revision is never
		# skipped for its variant set (glib2 2.88.3_2 exists only as
		# +dbus+x11; its dbus dependency is extracted but nothing links it).
		pkg_filename = max(pkgs, key=lambda f: self.pkg_sort_key(pkg_name, f))
		return pkg_filename, f'{pkg_base_url}/{pkg_filename}'

	@staticmethod
	def pkg_sort_key(pkg_name, pkg_filename):
		# name-version[_revision][+variant...].<platform>.<arch>.tbz2
		m = re.match(re.escape(pkg_name) +
		             r'-(?P<ver>[^_+]+?)(?:_(?P<rev>\d+))?(?P<var>(?:\+[\w.\-]+)*)'
		             r'\.(?:any_any|darwin_any|darwin_\d+)\.', pkg_filename)
		if not m:
			return ((), 0, 0, pkg_filename)
		ver = tuple(int(x) for x in re.findall(r'\d+', m.group('ver')))
		rev = int(m.group('rev') or 0)
		nvariants = m.group('var').count('+')
		return (ver, rev, -nvariants, pkg_filename)

	def is_pkg_installed(self, pkg_name):
		if not os.path.exists(self._installed_path):
			return False
		with open(self._installed_path) as f:
			installed = [l.strip().split('=')[0] for l in f.readlines()]
			return pkg_name in installed

	def mark_pkg_installed(self, pkg_name, pkg_version):
		if self.is_pkg_installed(pkg_name):
			return
		with open(os.path.join(self._extract_path, 'INSTALLED'), 'a+') as f:
			f.write(f'{pkg_name}={pkg_version}\n')

	def download_file(self, desc, url, dst):
		if os.path.exists(dst):
			print(f'    [+] Already have {desc}')
		else:
			print(f'    [+] Downloading {desc}')
			with open(dst, 'wb') as f:
				f.write(urlopen(url).read())

	def verify_pkg(self, pkg_path, sig_path):
		PUBKEYURL="https://svn.macports.org/repository/macports/trunk/base/macports-pubkey.pem"
		PUBKEYRMD160="d3a22f5be7184d6575afcc1be6fdb82fd25562e8"
		PUBKEYSHA1="214baa965af76ff71187e6c1ac91c559547f48ab"
		key_filename = 'macports-pubkey.pem'
		dst_key_filename = os.path.join(self._pkgs_path, key_filename)
		self.download_file('MacPorts key', PUBKEYURL, dst_key_filename)
		rmd160 = subprocess.run('openssl rmd160 "' + dst_key_filename + "\" | awk '{print $2}'",
	                            capture_output=True, shell=True,
	                            check=True).stdout.decode('utf-8').strip()
		sha1 = subprocess.run('openssl sha1 "' + dst_key_filename + "\" | awk '{print $2}'",
	                            capture_output=True, shell=True,
	                            check=True).stdout.decode('utf-8').strip()
		assert (rmd160 == PUBKEYRMD160 and sha1 == PUBKEYSHA1), 'Invalid MacPorts key'
		sha1 = subprocess.run('openssl dgst -ripemd160 '
			                  f'-verify "{dst_key_filename}" '
			                  f'-signature "{sig_path}" "{pkg_path}"',
	                            shell=True, check=True)

	def is_pkg_skipped(self, pkg_name):
		return any(pkg_name.startswith(n) for n in ('python', 'ncurses', 'mesa', 'llvm', 'libsndfile'))

	def install_pkg(self, pkg_name):
		if self.is_pkg_installed(pkg_name):
			print(f'[*] Package {pkg_name} already installed')
			return

		if self.is_pkg_skipped(pkg_name):
			print(f'[*] Skipping package {pkg_name}')
			return

		print(f'[*] Fetching {pkg_name}')
		pkg_filename, pkg_url = self.get_latest_pkg_filename_url(pkg_name)
		pkg_version = pkg_filename[re.search(r'-\d', pkg_filename).span()[0]+1:]
		pkg_version = pkg_version[:pkg_version.find('.'+self._darwin_target)]
		dst_pkg_filename = os.path.join(self._pkgs_path, pkg_filename)
		print(f'    [*] Found package {pkg_filename}')
		self.download_file(pkg_filename, pkg_url, dst_pkg_filename)

		dst_pkg_sig_filename = dst_pkg_filename + '.rmd160'
		pkg_sig_url = pkg_url + '.rmd160'
		self.download_file('package signature', pkg_sig_url, dst_pkg_sig_filename)

		print(f'    [+] Verifying package')
		self.verify_pkg(dst_pkg_filename, dst_pkg_sig_filename)

		print(f'    [+] Looking for dependencies')
		tb = TarFile.open(dst_pkg_filename)
		pkg_contents_file = tb.extractfile('./+CONTENTS').read().decode('utf-8')
		for dep in re.findall(r'@pkgdep (.+)', pkg_contents_file):
			print(f'        [>] {dep}')
			s = re.search(r'-\d', dep)
			if s:
				dep = dep[0:s.span()[0]]
			self._queue.append(dep)

		print(f'    [*] Checking tarball...')

		for member in tb.getmembers():
			extracted_path = os.path.abspath(os.path.join(self._extract_path, member.name))
			assert extracted_path.startswith(self._extract_path), f'tarball has a global file: {member.name}'
			# A refresh (its line removed from INSTALLED) extracts over the
			# old build, whose files MacPorts ships read-only: replace them.
			if not member.isdir() and os.path.lexists(extracted_path) \
					and not os.path.isdir(extracted_path):
				os.unlink(extracted_path)

		print(f'    [*] Extracting to {self._extract_path}')
		extract_kwargs = {'numeric_owner': True}
		if hasattr(tarfile, 'tar_filter'):
			# Python 3.14 made the 'data' extraction filter the default; it
			# refuses symlinks to absolute paths, and MacPorts packages carry
			# them (dbus: ./Library/LaunchDaemons/*.plist -> /opt/local/...).
			# 'tar' keeps the path-traversal checks (the loop above already
			# asserts every member stays under the extract root) while
			# allowing those links, which is what every earlier Python did.
			extract_kwargs['filter'] = 'tar'
		tb.extractall(self._extract_path, **extract_kwargs)

		for fpath in tb.getnames():
			# FIXME: Symlinks
			extracted_path = os.path.abspath(os.path.join(self._extract_path, fpath))
			if extracted_path.endswith('.pc'):
				print(f'    [*] Fixing {extracted_path}')
				with open(extracted_path, 'r') as f:
					lines = f.readlines()
				for i, l in enumerate(lines):
					if l.strip().startswith('prefix'):
						new_prefix = f'prefix={self._extract_path}/opt/local\n'
						if pkg_name.startswith('openssl'): # FIXME
							new_prefix = f'prefix={self._extract_path}/opt/local/libexec/openssl11\n'
						lines[i] = new_prefix
					elif l.strip().startswith('Requires.private:'):
						if pkg_name.startswith('libepoxy'):
							lines[i] = ''
				with open(extracted_path, 'w') as f:
					f.write(''.join(lines))

		if pkg_name == 'glib2':
			fpath = './opt/local/include/glib-2.0/glib/gi18n.h'
			extracted_path = os.path.abspath(os.path.join(self._extract_path, fpath))
			print(f'    [*] Fixing {extracted_path}')
			with open(extracted_path, 'r') as f:
				lines = f.read()
			s = '/opt/local/include/libintl.h'
			lines = lines.replace(s, self._extract_path + s)
			with open(extracted_path, 'w') as f:
				f.write(lines)

		self.mark_pkg_installed(pkg_name, pkg_version)
		self._installed.append(pkg_name)

	def check_installed_drift(self):
		"""Warn when the mirror has a newer build of an installed package.

		install_pkg() keeps whatever INSTALLED lists, while CI starts empty and
		takes the newest builds, so a local tree silently falls behind what
		releases ship. This walks INSTALLED, dependencies included, and only
		warns: refreshing under a developer would change the libraries
		between the two arms of an A/B.
		"""
		if not os.path.exists(self._installed_path):
			return
		with open(self._installed_path) as f:
			installed = [l.strip().split('=', 1) for l in f if '=' in l]
		behind = []
		for name, ver in installed:
			if name in self._installed:
				continue
			try:
				pkgs = self.list_pkgs(name, timeout=5)
			except HTTPError as e:
				# The mirror answered: only this port is missing or renamed
				print(f'[*] Skipping the MacPorts version check of {name}: {e}')
				continue
			except Exception as e:
				# Offline or a slow mirror: one timeout, not one per package
				print(f'[*] Skipping the MacPorts version check: {e}')
				return
			if not pkgs:
				continue
			newest = max(pkgs, key=lambda f: self.pkg_sort_key(name, f))
			have = self.pkg_sort_key(name, f'{name}-{ver}.{self._darwin_target}.{self._arch}.tbz2')
			# Version and revision only: a variant set is not a newer build
			if self.pkg_sort_key(name, newest)[:2] > have[:2]:
				newest_ver = re.sub(r'\.(?:any_any|darwin_any|darwin_\d+)\.\w+\.tbz2$', '', newest)[len(name) + 1:]
				behind.append(f'{name} {ver} -> {newest_ver}')
		if behind:
			print(f'*** Warning: macos-libs/{self._arch} is behind the MacPorts mirror CI builds from: '
			      + ', '.join(behind)
			      + f'; refresh with: rm macos-libs/{self._arch}/INSTALLED and rerun ./build.sh'
			      ' (between A/B campaigns, not between the arms of one)')

	def repair_pc_prefixes(self):
		"""Rewrite stale prefix= lines in .pc files when the tree has moved."""
		pc_dir = os.path.join(self._extract_path, 'opt', 'local', 'lib', 'pkgconfig')
		if not os.path.isdir(pc_dir):
			return
		expected_prefix = f'{self._extract_path}/opt/local'
		for pc_path in glob.glob(os.path.join(pc_dir, '*.pc')):
			with open(pc_path, 'r') as f:
				lines = f.readlines()
			needs_fix = False
			for i, l in enumerate(lines):
				if l.startswith('prefix=') and l.strip() != f'prefix={expected_prefix}':
					lines[i] = f'prefix={expected_prefix}\n'
					needs_fix = True
					break
			if needs_fix:
				print(f'[*] Repairing stale prefix in {os.path.basename(pc_path)}')
				with open(pc_path, 'w') as f:
					f.write(''.join(lines))

	def repair_gi18n_include(self):
		"""Re-point gi18n.h's absolute libintl.h include when the tree has moved.

		install_pkg() bakes the extract path into this include when glib2 is
		first unpacked, so moving the checkout leaves it pointing at the old
		location and every C++ file that includes gi18n.h fails to compile.
		"""
		gi18n = os.path.join(self._extract_path, 'opt', 'local', 'include',
		                     'glib-2.0', 'glib', 'gi18n.h')
		if not os.path.isfile(gi18n):
			return
		suffix = '/opt/local/include/libintl.h'
		with open(gi18n, 'r') as f:
			text = f.read()
		fixed = re.sub(r'#include <([^>]*)' + re.escape(suffix) + '>',
		               '#include <' + self._extract_path + suffix + '>', text)
		if fixed != text:
			print('[*] Repairing stale libintl.h include in gi18n.h')
			with open(gi18n, 'w') as f:
				f.write(fixed)

	def install_pkgs(self, requested):
		self.repair_pc_prefixes()
		self.repair_gi18n_include()
		self._queue.extend(requested)
		while len(self._queue) > 0:
			pkg_name = self._queue.pop(0)
			self.install_pkg(pkg_name)
		self.check_installed_drift()

def main():
	import argparse
	ap = argparse.ArgumentParser()
	ap.add_argument('arch', choices=('arm64', 'x86_64'))
	args = ap.parse_args()
	li = LibInstaller(args.arch)
	li.install_pkgs([
		'SDL3',
		'glib2',
		'libpixman',
		'libepoxy',
		'libpcap',
		'libslirp',
		'libusb'])

if __name__ == '__main__':
	main()