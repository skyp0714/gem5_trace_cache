from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class CXLController(SimObject):
    type = "CXLController"
    cxx_header = "cxl_objects/cxl_card.hh"
    cxx_class = "gem5::CXLController"

    # Parameter for the trace file path, with default
    trace_file = Param.String(
        "/home/hnpark2/traceCache/gem5/src/cxl_objects/trace/simple_input.txt",
        "Path to the trace file",
    )

    # Port to connect to the cache
    cache_port = RequestPort("Port to the cache")

    # Port to connect directly to memory (for cache misses)
    mem_port = RequestPort("Direct port to memory")

    # Port to connect to translation memory
    translation_port = RequestPort("Port for address translation")

    # Cache line size (for making requests)
    cache_line_size = Param.Int(64, "Cache line size in bytes")
