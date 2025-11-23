sysdep_supported_options = {
	'posix': false,
	'linux': false,
	'glibc': false,
	'bsd': false,
}

# libc will link against libboron.so.  rtld doesn't have
# this 'luxury' (it features only the bare minimum), and
# as such, we'll need to include some libboron.so sources
# instead.
rtld_sources += files(
	'generic/rtld_calls.S',
	'generic/rtld_csect.c',
	'generic/boron.cpp',
)

# note: links to ld.so's symbols for now
libc_sources += files(
	'generic/boron.cpp',
)

libc_deps += meson.get_compiler('c').find_library('boron', required: true)

if not no_headers
	install_headers(
		'include/abi-bits/access.h',
		'include/abi-bits/auxv.h',
		'include/abi-bits/seek-whence.h',
		'include/abi-bits/vm-flags.h',
		'include/abi-bits/errno.h',
		'include/abi-bits/fcntl.h',
		'include/abi-bits/in.h',
		'include/abi-bits/stat.h',
		'include/abi-bits/statx.h',
		'include/abi-bits/signal.h',
		'include/abi-bits/reboot.h',
		'include/abi-bits/resource.h',
		'include/abi-bits/socket.h',
		'include/abi-bits/termios.h',
		'include/abi-bits/time.h',
		'include/abi-bits/blkcnt_t.h',
		'include/abi-bits/blksize_t.h',
		'include/abi-bits/dev_t.h',
		'include/abi-bits/gid_t.h',
		'include/abi-bits/ino_t.h',
		'include/abi-bits/mode_t.h',
		'include/abi-bits/nlink_t.h',
		'include/abi-bits/pid_t.h',
		'include/abi-bits/uid_t.h',
		'include/abi-bits/wait.h',
		'include/abi-bits/limits.h',
		'include/abi-bits/utsname.h',
		'include/abi-bits/ptrace.h',
		'include/abi-bits/poll.h',
		'include/abi-bits/epoll.h',
		'include/abi-bits/packet.h',
		'include/abi-bits/inotify.h',
		'include/abi-bits/clockid_t.h',
		'include/abi-bits/ipc.h',
		'include/abi-bits/shm.h',
		'include/abi-bits/mqueue.h',
		'include/abi-bits/suseconds_t.h',
		'include/abi-bits/fsfilcnt_t.h',
		'include/abi-bits/fsblkcnt_t.h',
		'include/abi-bits/socklen_t.h',
		'include/abi-bits/statfs.h',
		'include/abi-bits/statvfs.h',
		'include/abi-bits/ioctls.h',
		'include/abi-bits/xattr.h',
		'include/abi-bits/msg.h',
		'include/abi-bits/vt.h',
		'include/abi-bits/random.h',
		'include/abi-bits/rlim_t.h',
		'include/abi-bits/sigval.h',
		'include/abi-bits/sigevent.h',
		'include/abi-bits/utmpx.h',
		'include/abi-bits/utmp-defines.h',
		subdir: 'abi-bits',
		follow_symlinks: true,
	)
	install_headers(
		'include/boron/rtl/ansi.h',
		'include/boron/rtl/assert.h',
		'include/boron/rtl/check64.h',
		'include/boron/rtl/cmdline.h',
		'include/boron/rtl/elf.h',
		'include/boron/rtl/fbsdtree.h',
		'include/boron/rtl/list.h',
		'include/boron/rtl/rbtree.h',
		'include/boron/rtl/symdefs.h',
		'include/boron/boron.h',
		'include/boron/atom.h',
		'include/boron/csect.h',
		'include/boron/eb.h',
		'include/boron/elf.h',
		'include/boron/env.h',
		'include/boron/exs.h',
		'include/boron/handle.h',
		'include/boron/heap.h',
		'include/boron/io.h',
		'include/boron/ioctl.h',
		'include/boron/ios.h',
		'include/boron/kes.h',
		'include/boron/list.h',
		'include/boron/main.h',
		'include/boron/mms.h',
		'include/boron/obs.h',
		'include/boron/proc.h',
		'include/boron/pss.h',
		'include/boron/status.h',
		'include/boron/string.h',
		'include/boron/svcs.h',
		'include/boron/ttys.h',
		subdir: 'boron',
		follow_symlinks: true,
	)
endif

if not headers_only
	crt = custom_target('crt0',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt-x86_64/crt0.S',
		output: 'crt0.o',
		install: true,
		install_dir: get_option('libdir')
	)

	custom_target('crti',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt-x86_64/crti.S',
		output: 'crti.o',
		install: true,
		install_dir: get_option('libdir')
	)

	custom_target('crtn',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt-x86_64/crtn.S',
		output: 'crtn.o',
		install: true,
		install_dir: get_option('libdir')
	)
endif
