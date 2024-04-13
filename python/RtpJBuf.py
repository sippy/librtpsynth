from ctypes import c_void_p, c_int, c_size_t, c_uint32, c_uint16, c_uint64, \
  POINTER, Union, Structure, create_string_buffer, addressof, string_at, cast

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
        ("lseq_end", c_uint64),
        ("ts_diff", c_uint32),
    ]

class RTPFrameType():
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
        ("type", c_int),
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
_rsth.rtpjbuf_flush.argtypes = [c_void_p]
_rsth.rtpjbuf_flush.restype = RJBUdpInR

class ERSFrame():
    lseq_start: int
    lseq_end: int
    ts_diff: int
    type = RTPFrameType.ERS

    def __init__(self, content):
        self.lseq_start = content.frame.ers.lseq_start
        self.lseq_end = content.frame.ers.lseq_end
        self.ts_diff = content.frame.ers.ts_diff

class RTPParseError(Exception):
    pass

class FrameWrapper():
    _rsth = None
    content = None
    data = None
    rtp_data: bytes

    def __init__(self, _rsth, content: RTPFrame, data):
        self._rsth = _rsth
        if content.type == RTPFrameType.ERS:
            self.content = ERSFrame(content)
        else:
            self.content = content
            rtp_data = cast(content.frame.rtp.data + content.frame.rtp.info.data_offset, c_void_p)
            self.rtp_data = string_at(rtp_data, content.frame.rtp.info.data_size)
        self.data = data

    def __del__(self):
        if self.content.type == RTPFrameType.RTP:
            self._rsth.rtpjbuf_frame_dtor(addressof(self.content))

    def __str__(self):
        if self.content.type == RTPFrameType.RTP:
            return f'RTP_Frame(seq={self.content.frame.rtp.lseq})'
        return f'RTP_Erasure(seq_range={self.content.lseq_start} ' + \
          f'-- {self.content.lseq_end})'

    def __repr__(self):
        return self.__str__()

RTP_PARSER_OK = 1

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
        return self._proc_RJBUdpInR(rval, (buffer, data))

    def _proc_RJBUdpInR(self, rval, bdata = None):
        if rval.error != 0:
            if rval.error < RTP_PARSER_OK:
                raise RTPParseError(f'rtpjbuf_udp_in(): error {rval.error}')
            raise RuntimeError(f'rtpjbuf_udp_in(): error {rval.error}')
        if bdata is not None:
            self._ref_cache[addressof(bdata[0])] = bdata
        ready = []
        for i, bucket in enumerate((rval.ready, rval.drop)):
            while bool(bucket):
                current = bucket.contents
                if current.type == RTPFrameType.RTP:
                    buffer, data = self._ref_cache.pop(current.frame.rtp.data)
                else:
                    data = None
                if i == 0:
                    ready.append(FrameWrapper(self._rsth, current, data))
                else:
                    assert current.type == RTPFrameType.RTP
                    self._rsth.rtpjbuf_frame_dtor(addressof(current))
                #print(current.frame.rtp.data, addressof(buffer))
                bucket = current.next
        return ready

    def flush(self):
        rval = self._rsth.rtpjbuf_flush(self._hndl)
        rval = self._proc_RJBUdpInR(rval)
        assert len(self._ref_cache.keys()) == 0
        return rval

    def __del__(self):
        if bool(self._hndl):
            self._rsth.rtpjbuf_dtor(self._hndl)
