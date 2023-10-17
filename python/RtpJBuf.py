from ctypes import c_void_p, c_int, c_size_t, c_uint32, c_uint16, c_uint64, \
  POINTER, Union, Structure, create_string_buffer, addressof

from .RtpSynth import _rsth

class RTPInfo(Structure):
    _fields_ = [
        ("data_size", c_size_t),
        ("data_offset", c_int),
        ("nsamples", c_int),
        ("ts", c_uint32),
        ("seq", c_uint16),
        ("ssrc", c_uint32),
        ("appendable", c_int),
        ("rtp_profile", c_void_p)  # replace with actual type
    ]

class RTPPacket(Structure):
    _fields_ = [
        ("info", RTPInfo),
        ("lseq", c_uint64),
        ("data", c_void_p)
    ]

class ERSFrame(Structure):
    _fields_ = [
        ("lseq_start", c_uint64),
        ("lseq_end", c_uint64)
    ]

class RTPFrameType(c_int):
    # replace with actual enum values
    RTP = 0
    ERS = 1

class RTPFrameUnion(Union):
    _fields_ = [
        ("rtp", RTPPacket),
        ("ers", ERSFrame)
    ]

class RTPFrame(Structure):
    pass

RTPFrame._fields_ = [
        ("type", RTPFrameType),
        ("frame", RTPFrameUnion),
        ("next", POINTER(RTPFrame))
    ]

class RJBUdpInR(Structure):
    _fields_ = [
        ("error", c_int),
        ("ready", POINTER(RTPFrame)),
        ("drop", POINTER(RTPFrame)),
    ]

_rsth.rtpjbuf_ctor.argtypes = [c_int]
_rsth.rtpjbuf_ctor.restype = c_void_p
_rsth.rtpjbuf_dtor.argtypes = [c_void_p,]
_rsth.rtpjbuf_frame_dtor.argtypes = [c_void_p,]
_rsth.rtpjbuf_udp_in.argtypes = [c_void_p, c_void_p, c_size_t]
_rsth.rtpjbuf_udp_in.restype = RJBUdpInR

class FrameWrapper():
    _rsth = None
    frame = None
    data = None

    def __init__(self, _rsth, frame, data):
        self._rsth = _rsth
        self.frame = frame

    def __del__(self):
        self._rsth.rtpjbuf_frame_dtor(addressof(self.frame))

class RtpJBuf(object):
    _hndl = None
    _rsth = None
    _ref_cache = None

    def __init__(self, capacity):
        self._rsth = _rsth
        self._hndl = self._rsth.rtpjbuf_ctor(capacity)
        if not bool(self._hndl):
            raise Exception('rtpjbuf_ctor() failed')
        self._ref_cache = {}

    def udp_in(self, data):
        buffer = create_string_buffer(data)
        size = len(data)
        rval = self._rsth.rtpjbuf_udp_in(self._hndl, buffer, size)
        if rval.error != 0:
            raise RuntimeError(f'rtpjbuf_udp_in(): error {rval.error}')
        self._ref_cache[addressof(buffer)] = (buffer, data)
        ready = []
        for i, bucket in enumerate((rval.ready, rval.drop)):
            while bool(bucket):
                current = bucket.contents
                buffer, data = self._ref_cache.pop(current.frame.rtp.data)
                if i == 0:
                    ready.append(FrameWrapper(self._rsth, current, data))
                else:
                    self._rsth.rtpjbuf_frame_dtor(addressof(current))
                #print(current.frame.rtp.data, addressof(buffer))
                bucket = current.next
        return ready

    def __del__(self):
        if bool(self._hndl):
            self._rsth.rtpjbuf_dtor(self._hndl)
