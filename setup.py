#!/usr/bin/env python

import os

from distutils.core import setup
from distutils.core import Extension
from sysconfig import get_config_var, get_platform

from build_tools.RunCTest import RunCTest
from build_tools.CheckVersion import CheckVersion

is_win = get_platform().startswith('win')
is_mac = get_platform().startswith('macosx-')

rtpsynth_ext_srcs = ['python/RtpSynth_mod.c', 'src/rtpsynth.c', 'src/rtp.c']
rtpjbuf_ext_srcs = ['python/RtpJBuf_mod.c', 'src/rtp.c', 'src/rtpjbuf.c']
rtpserver_ext_srcs = ['python/RtpServer_mod.c', 'src/SPMCQueue.c']
rtputils_ext_srcs = ['python/RtpUtils_mod.c']

extra_compile_args = ['-Wall']
if not is_win:
    cc = (get_config_var('CC') or '').lower()
    extra_compile_args += ['--std=c11', '-flto', '-pedantic']
    if 'clang' in cc:
        extra_compile_args.append('-Wno-zero-length-array')
else:
    extra_compile_args.append('/std:clatest')
extra_link_args = ['-flto'] if not is_win else []
debug_opts = (('-g3', '-O0'))
nodebug_opts = ('-O3',) if not is_win else ()

if get_platform() == 'linux-x86_64':
    # This is to disable x86-64-v2, see
    # https://github.com/pypa/manylinux/issues/1725
    extra_compile_args.append('-march=x86-64')

if False:
    extra_compile_args.extend(debug_opts)
    extra_link_args.extend(debug_opts)
else:
    extra_compile_args.extend(nodebug_opts)
    extra_link_args.extend(nodebug_opts)

rtpjbuf_link_args = extra_link_args.copy()
if is_mac:
    rtpjbuf_link_args.extend(['-undefined', 'dynamic_lookup'])

rtpsynth_link_args = extra_link_args.copy()
if is_mac:
    rtpsynth_link_args.extend(['-undefined', 'dynamic_lookup'])

module1 = Extension('rtpsynth.RtpSynth', sources = rtpsynth_ext_srcs, \
    include_dirs = ['src'], \
    extra_link_args = rtpsynth_link_args, \
    extra_compile_args = extra_compile_args)

module2 = Extension('rtpsynth.RtpJBuf', sources = rtpjbuf_ext_srcs, \
    include_dirs = ['src'], \
    extra_link_args = rtpjbuf_link_args, \
    extra_compile_args = extra_compile_args)

module3 = Extension('rtpsynth.RtpUtils', sources = rtputils_ext_srcs, \
    include_dirs = ['src'], \
    extra_link_args = rtpjbuf_link_args, \
    extra_compile_args = extra_compile_args)

module4 = None
if not is_win:
    module4 = Extension('rtpsynth.RtpServer', sources = rtpserver_ext_srcs, \
        include_dirs = ['src'], \
        extra_link_args = rtpjbuf_link_args, \
        extra_compile_args = extra_compile_args)

RunCTest.extra_link_args = extra_link_args.copy()
RunCTest.extra_compile_args = extra_compile_args

def get_ex_mod():
    if 'NO_PY_EXT' in os.environ:
        return None
    modules = [module1, module2, module3]
    if module4 is not None:
        modules.append(module4)
    return modules

with open("README.md", "r") as fh:
    long_description = fh.read()

kwargs = {'name':'rtpsynth',
      'version':'1.2.1',
      'description':'Library optimized to generate/process sequence of the RTP packets',
      'long_description': long_description,
      'long_description_content_type': "text/markdown",
      'author':'Maksym Sobolyev',
      'author_email':'sobomax@gmail.com',
      'url':'https://github.com/sippy/librtpsynth.git',
      'packages':['rtpsynth',],
      'package_dir':{'rtpsynth':'python'},
      'ext_modules': get_ex_mod(),
      'cmdclass': {'runctest': RunCTest, 'checkversion': CheckVersion},
      'license': 'BSD-2-Clause',
      'classifiers': [
            'Operating System :: POSIX',
            'Programming Language :: C',
            'Programming Language :: Python'
      ]
     }

if __name__ == '__main__':
    setup(**kwargs)
