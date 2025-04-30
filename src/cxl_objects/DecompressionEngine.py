from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class DecompressionEngine(ClockedObject):
    type = "DecompressionEngine"
    cxx_header = "cxl_objects/decompression_engine.hh"
    cxx_class = "gem5::DecompressionEngine"

    # Ports
    cxl_side_port = ResponsePort("Port connected to the CXL controller")
    mem_side_port = RequestPort("Port connected to memory")

    # Parameters
    # Note: decompression_latency is still kept for backward compatibility,
    # but is no longer used. Latency is now calculated based on block size.
    decompression_latency = Param.Cycles(
        100, "Number of cycles for backward compatibility (no longer used)"
    )
    block_size = Param.Int(64, "Compression block size in bytes (64B to 4KB)")
