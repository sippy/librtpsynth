#!/usr/bin/env python

from distutils.core import setup

kwargs = {'name':'RtpSynth',
      'version':'1.0',
      'description':'Library optimized to generate sequence of the RTP packets',
      'author':'Maksym Sobolyev',
      'author_email':'sobomax@gmail.com',
      'url':'https://github.com/sippy/librtpsynth.git',
      'packages':['librtpsynth',],
      'package_dir':{'librtpsynth':'python'}
     }

import sys

if __name__ == '__main__':
    setup(**kwargs)

