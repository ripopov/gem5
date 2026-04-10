from m5.params import *
from m5.SimObject import *


class FstTrace(SimObject):
    type = "FstTrace"
    cxx_header = "sim/fst_trace/fst_trace.hh"
    cxx_class = "gem5::FstTrace"

    cxx_exports = [PyBindMethod("setDumpActive")]

    trace_file = Param.String("trace.fst", "Output file name")
    compression = Param.String("lz4", "Compression: zlib, lz4, fastlz")
    timescale = Param.Int(-12, "FST timescale exponent (-12 = 1ps)")
    start_active = Param.Bool(True, "Start with dump enabled")
    use_work_item_roi = Param.Bool(
        False, "Toggle tracing from work item ROI markers when available"
    )
    stat_sample_period = Param.Tick(
        0, "Stat sampling period in ticks (0 = disabled)"
    )
