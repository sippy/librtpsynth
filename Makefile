LIB=	rtpsynth

SRCS=	rtpsynth.c rtpsynth.h rtp.h

SHLIB_MAJOR=	1
MK_PROFILE=	no

.include <bsd.lib.mk>
