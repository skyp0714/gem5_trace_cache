# Simple config file to run the CXLController SimObject

import argparse
import math  # Add math for log2
import os

import m5

# Import the debug module
import m5.debug
from m5.objects import *

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_CXL_OBJECTS_DIR = os.path.dirname(_THIS_DIR)
_DEFAULT_TRACE_FILE = os.path.join(
    _CXL_OBJECTS_DIR, "trace", "generated_trace.txt"
)
_DEFAULT_OUTPUT_FILE = os.path.join(
    _CXL_OBJECTS_DIR, "results", "cxl_latency_log.txt"
)
_DRAM_INTERFACES = {
    "DDR3_1600_8x8": DDR3_1600_8x8,
    "DDR4_2400_16x4": DDR4_2400_16x4,
    "DDR4_2400_8x8": DDR4_2400_8x8,
    "DDR4_2400_4x16": DDR4_2400_4x16,
    "DDR5_4400_4x8": DDR5_4400_4x8,
}


def is_power_of_two(value):
    return value > 0 and (value & (value - 1)) == 0


def build_channel_ranges(mem_range, num_channels, intlv_granularity):
    total_size = mem_range.size()
    if num_channels == 1:
        return [AddrRange(mem_range.start, size=total_size)], "contiguous"

    if is_power_of_two(num_channels):
        intlv_bits = int(math.log2(num_channels))
        intlv_low_bit = int(math.log2(intlv_granularity))
        ranges = []
        for match in range(num_channels):
            ranges.append(
                AddrRange(
                    mem_range.start,
                    size=total_size,
                    intlvHighBit=intlv_low_bit + intlv_bits - 1,
                    xorHighBit=0,
                    intlvBits=intlv_bits,
                    intlvMatch=match,
                )
            )
        return ranges, "striped"

    # gem5 AddrRange striping requires 2^N stripes. For non-power-of-two
    # channel counts, fall back to equal contiguous partitions.
    base_size = total_size // num_channels
    remainder = total_size % num_channels
    start = mem_range.start
    ranges = []
    for idx in range(num_channels):
        size = base_size + (1 if idx < remainder else 0)
        ranges.append(AddrRange(start, size=size))
        start += size
    return ranges, "contiguous"


# Parse command line arguments
parser = argparse.ArgumentParser()
parser.add_argument(
    "--trace-file",
    type=str,
    default=_DEFAULT_TRACE_FILE,
    help="Path to the trace file",
)
parser.add_argument(
    "--output-file",
    type=str,
    default=_DEFAULT_OUTPUT_FILE,
    help="Path to the output latency log file",
)
parser.add_argument(
    "--debug-flags",
    type=str,
    default="",
    help="Debug flags to enable (comma separated)",
)
parser.add_argument(
    "--enable-cxl-debug", action="store_true", help="Enable CXLCard debug flag"
)
parser.add_argument(
    "--enable-decomp-debug",
    action="store_true",
    help="Enable DecompressionEngine debug flag",
)
parser.add_argument(
    "--enable-cache-debug",
    action="store_true",
    help="Enable Cache debug flags",
)
parser.add_argument(
    "--enable-memory-debug",
    action="store_true",
    help="Enable Memory debug flags",
)
parser.add_argument(
    "--l1-assoc", type=int, default=8, help="L1 cache associativity"
)
parser.add_argument(
    "--l1-size", type=str, default="16MB", help="L1 cache size"
)
parser.add_argument(
    "--cacheline-size", type=int, default=64, help="Cache line size in bytes"
)
parser.add_argument(
    "--compression-block-size",
    type=int,
    default=4096,
    choices=[64, 128, 256, 512, 1024, 2048, 4096],
    help="Compression block size in bytes (64B to 4KB)",
)
parser.add_argument(
    "--dram-type",
    type=str,
    default="DDR4_2400_16x4",
    choices=sorted(_DRAM_INTERFACES.keys()),
    help="DRAM interface model used by the shared memory controllers",
)
parser.add_argument(
    "--mem-channels",
    type=int,
    default=4,
    help="Number of memory controllers behind the shared DRAM bus",
)
parser.add_argument(
    "--num-engines",
    type=int,
    default=8,
    help="Number of parallel decompression engines",
)
parser.add_argument(
    "--chunk-send-delay-ticks",
    type=int,
    default=3,
    help="Cycles between issuing chunk-generation events",
)
parser.add_argument(
    "--inter-memory-request-delay-ticks",
    type=int,
    default=1000,
    help="Cycles between memory requests sent by the decompression engine",
)
args = parser.parse_args()

# Enable requested debug flags
if args.debug_flags:
    for flag in args.debug_flags.split(","):
        m5.debug.flags[flag].enable()

# Enable CXLCard debug flag if requested
if args.enable_cxl_debug:
    m5.debug.flags["CXLCard"].enable()

# Enable DecompressionEngine debug flag if requested
if args.enable_decomp_debug:
    m5.debug.flags["DecompEngine"].enable()

# Enable Cache debug flags if requested
if args.enable_cache_debug:
    for cache_flag in ["Cache", "CachePort", "CacheRepl", "CacheTags"]:
        try:
            m5.debug.flags[cache_flag].enable()
            print(f"Enabled debug flag: {cache_flag}")
        except KeyError:
            print(f"Warning: Debug flag '{cache_flag}' not found")

# Enable Memory debug flags if requested
if args.enable_memory_debug:
    for mem_flag in ["Memory", "MemoryAccess", "MemCtrl", "DRAM"]:
        try:
            m5.debug.flags[mem_flag].enable()
            print(f"Enabled debug flag: {mem_flag}")
        except KeyError:
            print(f"Warning: Debug flag '{mem_flag}' not found")

# Create the root object
root = Root(full_system=False)

# Create a simplified system - avoid using power models
system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

# Disable DVS and power modeling
system.dvfs_handler.enable = False

# Create a simple memory system with just what we need
system.mem_mode = "timing"

# Use one shared DRAM address space for both data and translation lookups.
system.mem_ranges = [AddrRange("0GB", "64GB")]

shared_mem_channels = args.mem_channels
shared_intlv_granularity = 64  # 64B striping across the DRAM channels
shared_channel_ranges, shared_channel_mapping = build_channel_ranges(
    system.mem_ranges[0], shared_mem_channels, shared_intlv_granularity
)
engine_port_intlv_low_bit = int(math.log2(shared_intlv_granularity))
engine_port_intlv_bits = 2

system.shared_membus = SystemXBar(width=128, max_routing_table_size=4096)

# Create a simple cache
system.l1cache = Cache(
    size=args.l1_size,
    assoc=args.l1_assoc,
    tag_latency=30,  # 30ns @ 1GHz
    data_latency=30,  # 30ns @ 1GHz
    response_latency=30,  # 30ns effective hit path
    mshrs=16,
    tgts_per_mshr=20,
    write_buffers=16,
    cache_line_size=args.compression_block_size,
)

# Set system cache line size
system.cache_line_size = args.cacheline_size

# Create the CXL controller
system.cxl_controller = CXLController(
    trace_file=args.trace_file,
    output_file=args.output_file,
    block_size=args.compression_block_size,
    cache_line_size=args.cacheline_size,
    batch_size=1000,
)

# Connect the CXL controller to cache
system.cxl_controller.cache_port = system.l1cache.cpu_side

# Create memory controllers using the default DDR4-2400 timing model.
dram_iface_cls = _DRAM_INTERFACES[args.dram_type]
for i in range(shared_mem_channels):
    mem_ctrl = MemCtrl()
    mem_ctrl.dram = dram_iface_cls()
    mem_ctrl.dram.range = shared_channel_ranges[i]
    mem_ctrl.port = system.shared_membus.mem_side_ports
    setattr(system, f"shared_mem_ctrl_{i}", mem_ctrl)

# Connect the translation port to the shared DRAM bus.
system.cxl_controller.translation_port = system.shared_membus.cpu_side_ports

# Configure decompression engine with light chunk pacing.
system.decompression_engine = DecompressionEngine(
    block_size=args.compression_block_size,
    cache_line_size=args.cacheline_size,
    num_engines=args.num_engines,
    chunk_send_delay_ticks=args.chunk_send_delay_ticks,
    inter_memory_request_delay_ticks=args.inter_memory_request_delay_ticks,
    interleaving_low_bit=engine_port_intlv_low_bit,
    interleaving_bits=engine_port_intlv_bits,
)

# Connect decompression engine between CXL controller and decompression bus
system.cxl_controller.mem_port = system.decompression_engine.cxl_side_port

for i in range(4):
    port_name = f"mem_side_port_{i}"
    setattr(
        system.decompression_engine,
        port_name,
        system.shared_membus.cpu_side_ports,
    )

# Connect system port to the shared DRAM bus.
system.system_port = system.shared_membus.cpu_side_ports

# Set the system as the child of root
root.system = system

# Instantiate the simulation
m5.instantiate()

# Print the simulation configuration
print("Beginning CXL trace replay simulation")
print(f"Using trace file: {args.trace_file}")
print(
    f"Logging latency details to: {args.output_file}"
)  # Log output file path
print(
    f"L1 Cache: {args.l1_size}, {args.l1_assoc}-way, {system.l1cache.cache_line_size}B lines"  # Use actual L1 cache line size
)
print(f"Compression block size: {args.compression_block_size}B")
print(
    f"Decompression Engines: {system.decompression_engine.num_engines}"
)  # ADDED
print(f"Shared DRAM: {args.dram_type}")
print(f"Memory channels: {args.mem_channels} ({shared_channel_mapping})")
print(f"Chunk send delay ticks: {args.chunk_send_delay_ticks}")
print(
    f"Inter-memory request delay ticks: {args.inter_memory_request_delay_ticks}"
)
print(
    f"Inter-memory request delay ticks: {args.inter_memory_request_delay_ticks}"
)

# Run the simulation
exit_event = m5.simulate()

# Print exit status
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
