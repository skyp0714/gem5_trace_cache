from m5.params import *
from m5.objects.ClockedObject import ClockedObject

class DecompressionEngine(ClockedObject):
    type = 'DecompressionEngine'
    cxx_header = 'cxl_objects/decompression_engine.hh'
    cxx_class = 'gem5::DecompressionEngine'

    # Ports
    cxl_side_port = ResponsePort("Port connected to the CXL controller")
    mem_side_port = RequestPort("Port connected to memory")

    # Parameters
    decompression_latency = Param.Cycles(100, 
        "Number of cycles it takes to decompress data")
