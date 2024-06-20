# Copyright (c) 2018 Sippy Software, Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from ctypes import cdll, c_void_p, c_int, create_string_buffer, c_uint
from math import modf
import sys, os, site
from sysconfig import get_config_var, get_platform

from .env import RSTH_MOD_NAME

_esuf = get_config_var('EXT_SUFFIX')
if not _esuf:
    _esuf = '.so'
try:
    import pathlib
    _ROOT = str(pathlib.Path(__file__).parent.absolute())
except ImportError:
    _ROOT = os.path.abspath(os.path.dirname(__file__))
#print('ROOT: ' + str(_ROOT))
modloc = site.getsitepackages()
modloc.insert(0, os.path.join(_ROOT, ".."))
for p in modloc:
   try:
       #print("Trying %s" % os.path.join(p, RSTH_MOD_NAME + _esuf))
       _rsth = cdll.LoadLibrary(os.path.join(p, RSTH_MOD_NAME + _esuf))
   except:
       continue
   break
else:
   _rsth = cdll.LoadLibrary('librtpsynth.so')

_rsth.rsynth_ctor.argtypes = [c_int, c_int]
_rsth.rsynth_ctor.restype = c_void_p
_rsth.rsynth_dtor.argtypes = [c_void_p,]
_rsth.rsynth_next_pkt.restype = c_void_p
_rsth.rsynth_next_pkt.argtypes = [c_void_p, c_int, c_int]
_rsth.rsynth_next_pkt_pa.restype = c_int
_rsth.rsynth_next_pkt_pa.argtypes = [c_void_p, c_int, c_int, c_void_p, c_uint, c_int]
_rsth.rsynth_set_mbt.argtypes = [c_void_p, c_uint]
_rsth.rsynth_set_mbt.restype = c_uint
_rsth.rsynth_resync.argtypes = [c_void_p, c_void_p]
_rsth.rsynth_skip.argtypes = [c_void_p, c_int]

_rsth.rsynth_pkt_free.argtypes = [c_void_p,]


class RtpSynth(object):
    _hndl = None
    _rsth = None

    def __init__(self, srate, ptime):
        self._rsth = _rsth
        self._hndl = self._rsth.rsynth_ctor(srate, ptime)
        if not bool(self._hndl):
            raise Exception('rsynth_ctor() failed')

    def next_pkt(self, plen, pt, pload = None):
        pktlen = plen + 32
        if pload != None:
            pkt = create_string_buffer(pload, pktlen)
            filled = 1
        else:
            pkt = create_string_buffer(pktlen)
            filled = 0
        plen = self._rsth.rsynth_next_pkt_pa(self._hndl, plen, pt, pkt, pktlen, filled)
        return (pkt.raw[:plen])

    def pkt_free(self, pkt):
        self._rsth.rsynth_pkt_free(pkt)

    def set_mbt(self, mbt):
        return self._rsth.rsynth_set_mbt(self._hndl, mbt)

    def resync(self):
        self._rsth.rsynth_resync(self._hndl, None)

    def skip(self, n):
        self._rsth.rsynth_skip(self._hndl, n)

    def __del__(self):
        if bool(self._hndl):
            self._rsth.rsynth_dtor(self._hndl)

if __name__ == '__main__':
    from time import monotonic
    i = 0
    rs = RtpSynth(8000, 30)
    stime = monotonic()
    while monotonic() - stime < 5.0:
        if i % 43 == 0:
            rs.resync()
        rp = rs.next_pkt(170, 0)
        i += 1
        if i % 4242 == 0:
            res = rs.set_mbt(1)
            assert res == 0
            res = rs.set_mbt(0)
            assert res == 1
    del rs
