# Simple config file to run the CXLController SimObject

import m5
from m5.objects import *
import argparse
import os

# Import the debug module
import m5.debug

# Parse command line arguments
parser = argparse.ArgumentParser()
parser.add_argument("--trace-file", type=str, default="/home/hnpark2/traceCache/gem5/src/cxl_objects/trace/simple_input.txt",
                    help="Path to the trace file")
parser.add_argument("--debug-flags", type=str, default="",
                    help="Debug flags to enable (comma separated)")
parser.add_argument("--enable-cxl-debug", action="store_true",
                    help="Enable CXLCard debug flag")
parser.add_argument("--enable-decomp-debug", action="store_true",
                    help="Enable DecompressionEngine debug flag")
parser.add_argument("--enable-cache-debug", action="store_true",
                    help="Enable Cache debug flags")
parser.add_argument("--enable-memory-debug", action="store_true",
                    help="Enable Memory debug flags")
parser.add_argument("--l1-size", type=str, default="64kB",
                    help="L1 cache size")
parser.add_argument("--l1-assoc", type=int, default=8,
                    help="L1 cache associativity")
parser.add_argument("--cacheline-size", type=int, default=64,
                    help="Cache line size in bytes")
args = parser.parse_args()

# Enable requested debug flags
if args.debug_flags:
    for flag in args.debug_flags.split(','):
        m5.debug.flags[flag].enable()

# Enable CXLCard debug flag if requested
if args.enable_cxl_debug:
    m5.debug.flags['CXLCard'].enable()

# Enable DecompressionEngine debug flag if requested
if args.enable_decomp_debug:
    m5.debug.flags['DecompEngine'].enable()

# Enable Cache debug flags if requested
if args.enable_cache_debug:
    for cache_flag in ['Cache', 'CachePort', 'CacheRepl', 'CacheTags']:
        try:
            m5.debug.flags[cache_flag].enable()
            print(f"Enabled debug flag: {cache_flag}")
        except KeyError:
            print(f"Warning: Debug flag '{cache_flag}' not found")

# Enable Memory debug flags if requested
if args.enable_memory_debug:
    for mem_flag in ['Memory', 'MemoryAccess', 'MemCtrl', 'DRAM']:
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
system.clk_domain.clock = '1GHz'
system.clk_domain.voltage_domain = VoltageDomain()

# Disable DVS and power modeling
system.dvfs_handler.enable = False

# Create a simple memory system with just what we need
system.mem_mode = 'timing'
system.mem_ranges = [AddrRange('512MB')]

# Create the memory bus
system.membus = SystemXBar()

# Create a simple cache
system.l1cache = Cache(size=args.l1_size,
                      assoc=args.l1_assoc,
                      tag_latency=2,
                      data_latency=2,
                      response_latency=2,
                      mshrs=4,
                      tgts_per_mshr=20)

# Create the CXL controller with updated cache line size
system.cxl_controller = CXLController(trace_file=args.trace_file,
                                     cache_line_size=args.cacheline_size)

# Connect the CXL controller to cache
system.cxl_controller.cache_port = system.l1cache.cpu_side

# Connect memory according to configuration

# Create a memory controller and connect it to the memory bus
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

system.decompression_engine = DecompressionEngine()
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
print(f"L1 Cache: {args.l1_size}, {args.l1_assoc}-way, {args.cacheline_size}B lines")

# Run the simulation
exit_event = m5.simulate()

# Print exit status
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
