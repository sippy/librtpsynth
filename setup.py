#!/usr/bin/env python

import os

from distutils.core import setup
from distutils.core import Extension

from python.env import RSTH_MOD_NAME

rs_srcs = ['src/rtpsynth.c']

module1 = Extension(RSTH_MOD_NAME, sources = rs_srcs, \
    extra_link_args = ['-Wl,--version-script=src/Symbol.map',])

def get_ex_mod():
    if 'NO_PY_EXT' in os.environ:
        return None
    return [module1]

kwargs = {'name':'RtpSynth',
      'version':'1.0',
      'description':'Library optimized to generate sequence of the RTP packets',
      'author':'Maksym Sobolyev',
      'author_email':'sobomax@gmail.com',
      'url':'https://github.com/sippy/librtpsynth.git',
      'packages':['rtpsynth',],
      'package_dir':{'rtpsynth':'python'},
      'ext_modules': get_ex_mod(),
     }

import sys

if __name__ == '__main__':
    setup(**kwargs)

