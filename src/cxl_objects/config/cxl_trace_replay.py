# Simple config file to run the CXLController SimObject

import argparse
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
    tag_latency=2,
    data_latency=2,
    response_latency=2,
    mshrs=4,
    tgts_per_mshr=20,
)

# Set cache line size to match compression block size
system.cache_line_size = args.compression_block_size

# Create the CXL controller with updated cache line size and output file
system.cxl_controller = CXLController(
    trace_file=args.trace_file,
    output_file=args.output_file,  # Pass the output file path
    cache_line_size=args.compression_block_size,
)

# Connect the CXL controller to cache
system.cxl_controller.cache_port = system.l1cache.cpu_side

# Create a memory controller and connect it to the memory bus
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# Connect the translation port directly to the main membus
system.cxl_controller.translation_port = system.membus.cpu_side_ports

# Make sure decompression engine uses the same block size
system.decompression_engine = DecompressionEngine(
    block_size=args.compression_block_size
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
    f"L1 Cache: {args.l1_size}, {args.l1_assoc}-way, {args.compression_block_size}B lines"
)
print(f"Compression block size: {args.compression_block_size}B")

# Run the simulation
exit_event = m5.simulate()

# Print exit status
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
