from m5.params import *
from m5.SimObject import *


class FstTrace(SimObject):
    type = "FstTrace"
    cxx_header = "sim/fst_trace/fst_trace.hh"
    cxx_class = "gem5::FstTrace"

    trace_file = Param.String("message_buffers.fst", "Output file name")
    compression = Param.String("lz4", "Compression: zlib, lz4, fastlz")
    timescale = Param.Int(-12, "FST timescale exponent (-12 = 1ps)")
