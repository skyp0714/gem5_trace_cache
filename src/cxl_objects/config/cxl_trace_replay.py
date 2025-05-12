# Simple config file to run the CXLController SimObject

import argparse
import math  # Add math for log2
import os

import m5

# Import the debug module
import m5.debug
from m5.objects import *

# Parse command line arguments
parser = argparse.ArgumentParser()
parser.add_argument(
    "--trace-file",
    type=str,
    default="/home/hnpark2/traceCache/gem5/src/cxl_objects/trace/simple_input.txt",
    help="Path to the trace file",
)
parser.add_argument(
    "--output-file",
    type=str,
    default="/home/hnpark2/traceCache/gem5/src/cxl_objects/results/cxl_latency_log.txt",
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
    "--l1-size", type=str, default="32MB", help="L1 cache size"
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
system.mem_ranges = [AddrRange("64GB")]

# Create the memory bus
system.membus = SystemXBar()

# Create a simple cache
system.l1cache = Cache(
    size=args.l1_size,
    assoc=args.l1_assoc,
    tag_latency=50,
    data_latency=50,
    response_latency=50,
    mshrs=4,
    tgts_per_mshr=20,
    cache_line_size=args.compression_block_size,  # Pass integer directly
)

# Set cache line size. Note: args.cacheline_size (default 64) is defined.
# To meet DRAM interleaving requirements (interleaving granularity must be >= cache line size),
# system.cache_line_size should be consistent with the intlv_granularity (64B).
# We will use args.cacheline_size (default 64B) for the system's cache line size.
system.cache_line_size = (
    args.cacheline_size
)  # Set system-wide cache line size to args.cacheline_size (e.g., 64)

# Create the CXL controller with updated cache line size and output file
system.cxl_controller = CXLController(
    trace_file=args.trace_file,
    output_file=args.output_file,
    block_size=args.compression_block_size,
    cache_line_size=args.cacheline_size,
)

# Connect the CXL controller to cache
system.cxl_controller.cache_port = system.l1cache.cpu_side

# Configure memory controllers for 4 channels with 64B interleaving
num_mem_channels = 4
intlv_granularity = 64  # Bytes

# Calculate interleaving parameters
intlv_bits = int(math.log2(num_mem_channels))
intlv_low_bit = int(math.log2(intlv_granularity))
# xor_low_bit = 0 by default in MemConfig.py, so xorHighBit will be 0
xor_high_bit = (
    0  # Assuming no XORing for simplicity, or based on xor_low_bit = 0
)

# system.mem_ctrls = [] # REMOVE: System object does not have a 'mem_ctrls' parameter by default
_mem_controllers = (
    []
)  # Use a local list to keep track if needed for other Python logic
for i in range(num_mem_channels):
    mem_ctrl = MemCtrl()
    mem_ctrl.dram = DDR4_2400_8x8()  # Or any other DRAM type
    # Configure address range for interleaving
    mem_ctrl.dram.range = AddrRange(
        system.mem_ranges[0].start,
        size=system.mem_ranges[0].size(),
        intlvHighBit=intlv_low_bit + intlv_bits - 1,
        xorHighBit=xor_high_bit,
        intlvBits=intlv_bits,
        intlvMatch=i,
    )
    mem_ctrl.port = system.membus.mem_side_ports
    # Assign the memory controller to the system object with a unique name
    setattr(system, f"mem_ctrl_{i}", mem_ctrl)
    _mem_controllers.append(mem_ctrl)  # Keep in a local list if necessary

# Connect the translation port directly to the main membus
system.cxl_controller.translation_port = system.membus.cpu_side_ports

# Make sure decompression engine uses the same block size and num_engines
system.decompression_engine = DecompressionEngine(
    block_size=args.compression_block_size,
    cache_line_size=args.cacheline_size,
    num_engines=4,  # Set the number of engines (can be parameterized later if needed)
)
system.cxl_controller.mem_port = system.decompression_engine.cxl_side_port
system.decompression_engine.mem_side_port = system.membus.cpu_side_ports

# Set up the system port
system.system_port = system.membus.cpu_side_ports

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

# Run the simulation
exit_event = m5.simulate()

# Print exit status
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
