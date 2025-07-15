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
    default="/home/hnpark2/traceCache/gem5/src/cxl_objects/trace/generated_trace.txt",
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

# Define two separate memory ranges
system.mem_ranges = [
    AddrRange("0GB", "32GB"),  # For decompression (0-32GB)
    AddrRange("32GB", "64GB"),  # For translation (32-64GB)
]

# Create two separate memory buses
system.transbus = SystemXBar(
    width=64, max_routing_table_size=4096
)  # For translation

# Configure 4 memory controllers for decompression (0-32GB range)
decomp_mem_channels = 4
decomp_intlv_granularity = 64  # 64B interleaving granularity (explicit)

for i in range(decomp_mem_channels):
    setattr(
        system,
        f"decomp_xbar_{i}",
        SystemXBar(width=128, max_routing_table_size=4096),
    )

# Create a simple cache
system.l1cache = Cache(
    size=args.l1_size,
    assoc=args.l1_assoc,
    tag_latency=50,
    data_latency=50,
    response_latency=50,
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

# Calculate interleaving parameters for decompression
decomp_intlv_bits = int(math.log2(decomp_mem_channels))
decomp_intlv_low_bit = int(math.log2(decomp_intlv_granularity))
decomp_xor_high_bit = 0  # No XOR

# Create decompression memory controllers with lower latency
for i in range(decomp_mem_channels):
    mem_ctrl = MemCtrl()
    mem_ctrl.dram = DDR4_2400_16x4()  # Higher bandwidth memory
    mem_ctrl.dram.banks_per_rank = 32
    # Configure DRAM timing for lower latency
    mem_ctrl.dram.tCK = "0.5ns"  # 2GHz clock
    mem_ctrl.dram.tBURST = "2.5ns"  # Reduced burst time
    mem_ctrl.dram.tRCD = "10ns"  # Lower RCD
    mem_ctrl.dram.tCL = "10ns"  # Lower CAS latency
    mem_ctrl.dram.tRP = "10ns"  # Lower row precharge time
    mem_ctrl.dram.tRAS = "24ns"  # Lower row active time

    # Verify interleaving is correctly set to 64B
    mem_ctrl.dram.range = AddrRange(
        system.mem_ranges[0].start,
        size=system.mem_ranges[0].size(),
        intlvHighBit=decomp_intlv_low_bit + decomp_intlv_bits - 1,
        xorHighBit=decomp_xor_high_bit,
        intlvBits=decomp_intlv_bits,
        intlvMatch=i,
    )
    xbar = getattr(system, f"decomp_xbar_{i}")
    mem_ctrl.port = xbar.mem_side_ports
    setattr(system, f"decomp_mem_ctrl_{i}", mem_ctrl)

# Configure 2 memory controllers for translation (32-64GB range)
trans_mem_channels = 4
trans_intlv_granularity = 64  # 64B interleaving granularity (explicit)

# Calculate interleaving parameters for translation
trans_intlv_bits = int(math.log2(trans_mem_channels))
trans_intlv_low_bit = int(math.log2(trans_intlv_granularity))
trans_xor_high_bit = 0  # No XOR

# Create translation memory controllers with lower latency
for i in range(trans_mem_channels):
    mem_ctrl = MemCtrl()
    mem_ctrl.dram = DDR4_2400_16x4()  # Higher bandwidth memory
    # Configure DRAM timing for lower latency
    mem_ctrl.dram.tCK = "0.5ns"  # 2GHz clock
    mem_ctrl.dram.tBURST = "2.5ns"  # Reduced burst time
    mem_ctrl.dram.tRCD = "10ns"  # Lower RCD
    mem_ctrl.dram.tCL = "10ns"  # Lower CAS latency
    mem_ctrl.dram.tRP = "10ns"  # Lower row precharge time
    mem_ctrl.dram.tRAS = "24ns"  # Lower row active time

    # Verify interleaving is correctly set to 64B
    mem_ctrl.dram.range = AddrRange(
        system.mem_ranges[1].start,
        size=system.mem_ranges[1].size(),
        intlvHighBit=trans_intlv_low_bit + trans_intlv_bits - 1,
        xorHighBit=trans_xor_high_bit,
        intlvBits=trans_intlv_bits,
        intlvMatch=i,
    )
    mem_ctrl.port = system.transbus.mem_side_ports
    setattr(system, f"trans_mem_ctrl_{i}", mem_ctrl)

# Connect the translation port to translation bus
system.cxl_controller.translation_port = system.transbus.cpu_side_ports

# Configure decompression engine with reduced delay
system.decompression_engine = DecompressionEngine(
    block_size=args.compression_block_size,
    cache_line_size=args.cacheline_size,
    num_engines=4,
    chunk_send_delay_ticks=0,  # Reduced from 1000
    inter_memory_request_delay_ticks=0,  # Reduced from 5000
    interleaving_low_bit=decomp_intlv_low_bit,  # Match memory controller interleaving
    interleaving_bits=decomp_intlv_bits,  # Match memory controller interleaving
)

# Connect decompression engine between CXL controller and decompression bus
system.cxl_controller.mem_port = system.decompression_engine.cxl_side_port

for i in range(decomp_mem_channels):
    port_name = f"mem_side_port_{i}"
    xbar = getattr(system, f"decomp_xbar_{i}")

    # DecompressionEngine의 해당 포트를 xbar에 연결
    setattr(system.decompression_engine, port_name, xbar.cpu_side_ports)

# Connect system port to translation bus (for system access)
system.system_port = system.transbus.cpu_side_ports

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
