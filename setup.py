#!/usr/bin/env python

import os

from distutils.core import setup
from distutils.core import Extension
from sysconfig import get_platform

from python.env import RSTH_MOD_NAME
from setup.RunCTest import RunCTest
from setup.CheckVersion import CheckVersion

is_win = get_platform().startswith('win')
is_mac = get_platform().startswith('macosx-')

rs_srcs = ['src/rtpsynth.c', 'src/rtp.c', 'src/rtpjbuf.c']

if not is_mac:
    rs_srcs.append('python/RtpSynth_mod.c')

extra_compile_args = ['-Wall']
if not is_win:
    extra_compile_args += ['--std=c11', '-Wno-zero-length-array',
                           '-flto', '-pedantic']
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

module1 = Extension(RSTH_MOD_NAME, sources = rs_srcs, \
    extra_link_args = extra_link_args, \
    extra_compile_args = extra_compile_args)

RunCTest.extra_link_args = extra_link_args.copy()
RunCTest.extra_compile_args = extra_compile_args

if not is_mac and not is_win:
    extra_link_args.append('-Wl,--version-script=src/Symbol.map')

def get_ex_mod():
    if 'NO_PY_EXT' in os.environ:
        return None
    return [module1]

with open("README.md", "r") as fh:
    long_description = fh.read()

kwargs = {'name':'rtpsynth',
      'version':'1.2',
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
      'classifiers': [
            'License :: OSI Approved :: BSD License',
            'Operating System :: POSIX',
            'Programming Language :: C',
            'Programming Language :: Python'
      ]
     }

if __name__ == '__main__':
    setup(**kwargs)
