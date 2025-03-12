from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject

class CXLController(SimObject):
    type = 'CXLController'
    cxx_header = "cxl_objects/cxl_card.hh"
    cxx_class = "gem5::CXLController"
    
    # Parameter for the trace file path, with default
    trace_file = Param.String("/home/hnpark2/traceCache/gem5/src/cxl_objects/trace/simple_input.txt", 
                             "Path to the trace file")
    
    # Port to connect to the cache hierarchy - use explicit mem_side_port naming
    mem_side_port = RequestPort("Port to the memory system")
    
    # Cache line size (for making requests)
    cache_line_size = Param.Int(64, "Cache line size in bytes")
