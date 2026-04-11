from m5.params import *
from m5.SimObject import SimObject


class FtrTrace(SimObject):
    type = "FtrTrace"
    cxx_header = "sim/transaction_trace/ftr_trace.hh"
    cxx_class = "gem5::FtrTrace"

    output_format = Param.String("text", "Output format: 'text'")
    output_file = Param.String(
        "transactions", "Base filename (extension added automatically)"
    )
