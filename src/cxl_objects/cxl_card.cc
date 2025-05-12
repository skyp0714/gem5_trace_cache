#include "cxl_objects/cxl_card.hh"

#include <fstream>
#include <string>
#include <sstream>
#include <iomanip> // Include for std::setw, std::left, std::fixed, std::setprecision

#include "base/logging.hh"
#include "sim/core.hh"
// #include "sim/sim_clock.hh"     // Include for SimClock::Frequency - Reverted
#include "sim/stat_control.hh"
#include "debug/CXLCard.hh"
#include "sim/sim_exit.hh" // Include for registerExitCallback
#include "mem/packet_access.hh"

namespace gem5
{

// Define the stats group constructor (Counters only)
CXLController::CXLStats::CXLStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(totalRequests, "Total number of completed requests"),
      ADD_STAT(totalHits, "Total number of cache hits"),
      ADD_STAT(totalMisses, "Total number of cache misses"),
      ADD_STAT(readHits, "Number of read hits"),
      ADD_STAT(readMisses, "Number of read misses"),
      ADD_STAT(writeHits, "Number of write hits"),
      ADD_STAT(writeMisses, "Number of write misses")
      // Removed hitRate formula
{
    // No formulas or latency averages to initialize here
}

void
CXLRequestEvent::process()
{
    // Forward the request to the controller for processing
    controller->processRequest(getRequest());
}

CXLController::CXLController(const CXLControllerParams &p)
    : SimObject(p),
      traceFilePath(p.trace_file),
      outputFilePath(p.output_file), // Initialize output file path
      cacheLineSize(p.cache_line_size),
      blockSize(p.block_size),
      cachePort(name() + ".cache_port", this, true),
      memPort(name() + ".mem_port", this, false),
      translationPort(name() + ".translation_port", this, false),
      totalRequests(0), // This is total from trace file
      completedRequests(0), // This counts completed requests
      stats(this)
{
    // Open the output file
    outputFile.open(outputFilePath);
    if (!outputFile.is_open()) {
        fatal("Could not open CXL output log file: %s", outputFilePath);
    }

    // Write the header to the output file with fixed widths
    // Add ArrivalTime(us) column (width 15)
    outputFile << std::left << std::setw(18) << "Address"
               << std::setw(15) << "ArrivalTime(us)" // ADDED column
               << std::setw(22) << "CompressionRatio(%)"
               << std::setw(15) << "Latency(ns)"
               << std::setw(8) << "IsHit" << std::endl;
    // Adjust separator line length
    outputFile << std::string(18 + 15 + 22 + 15 + 8, '-') << std::endl; // Separator line

    // Initialize summary stats (already done via member initialization)
    totalLatencySum = 0.0;
    hitLatencySum = 0.0;
    missLatencySum = 0.0;
    hitCount = 0;
    missCount = 0;

    // Register dumpStats to be called when simulation exits
    registerExitCallback([this](){ dumpStats(); });
    DPRINTF(CXLCard, "Registered dumpStats exit callback.\n");
}


CXLController::~CXLController()
{
    // Summary writing moved to dumpStats() called via exit callback

    // Close the file if it's still open
    if (outputFile.is_open()) {
        outputFile.close();
    }

    // Clean up any packets in the retry queues
    while (!cacheRetryQueue.empty()) {
        PacketPtr pkt = cacheRetryQueue.front();
        cacheRetryQueue.pop();
        delete pkt;
    }

    while (!memRetryQueue.empty()) {
        PacketPtr pkt = memRetryQueue.front();
        memRetryQueue.pop();
        delete pkt;
    }

    while (!translationRetryQueue.empty()) {
        PacketPtr pkt = translationRetryQueue.front();
        translationRetryQueue.pop();
        delete pkt;
    }

    // Clean up any outstanding request packets
    for (auto& pair : outstandingReqs) {
        if (pair.second->pkt) {
            delete pair.second->pkt;
            pair.second->pkt = nullptr;
        }
        if (pair.second->memPkt) {
            delete pair.second->memPkt;
            pair.second->memPkt = nullptr;
        }
        if (pair.second->transPkt) {
            delete pair.second->transPkt;
            pair.second->transPkt = nullptr;
        }
    }

    // Clean up dependency tracking data structures
    dependentReqs.clear();
    outstandingMemReqs.clear();
    outstandingReqs.clear();
}

void
CXLController::dumpStats()
{
    DPRINTF(CXLCard, "dumpStats() called.\n");
    // Calculate average latencies
    double avgTotalLatency = (completedRequests > 0) ? (totalLatencySum / completedRequests) : 0.0;
    double avgHitLatency = (hitCount > 0) ? (hitLatencySum / hitCount) : 0.0;
    double avgMissLatency = (missCount > 0) ? (missLatencySum / missCount) : 0.0;

    // Write summary statistics to the output file with fixed widths
    if (outputFile.is_open()) {
        // Add separator before summary
        outputFile << "\n" << std::string(18 + 15 + 22 + 15 + 8, '-') << std::endl; // Adjusted width
        outputFile << "--- Summary ---" << std::endl;
        outputFile << std::fixed << std::setprecision(2); // Set precision for output

        outputFile << std::left << std::setw(25) << "Average Latency:"
                   << std::setw(15) << avgTotalLatency << " ns" << std::endl;
        outputFile << std::left << std::setw(25) << "Average Hit Latency:"
                   << std::setw(15) << avgHitLatency << " ns (" << hitCount << " hits)" << std::endl;
        outputFile << std::left << std::setw(25) << "Average Miss Latency:"
                   << std::setw(15) << avgMissLatency << " ns (" << missCount << " misses)" << std::endl;

        // Ensure data is flushed to the file
        outputFile.flush();
        DPRINTF(CXLCard, "Summary statistics written to %s.\n", outputFilePath);
    } else {
        warn("Output file stream was not open when dumpStats() was called.");
    }
}


void
CXLController::completeDependentRequests(CXLRequest* primaryReq) // Renamed parameter
{
    auto depIt = dependentReqs.find(primaryReq);
    if (depIt == dependentReqs.end()) {
        return;
    }

    // Get the list of dependent requests
    // Make a copy in case completeRequest modifies the map/vector indirectly
    std::vector<CXLRequest*> dependents_copy = depIt->second; // ADDED COPY

    DPRINTF(CXLCard, "Completing %u dependent requests for primary addr 0x%lx (req %p)\n",
            dependents_copy.size(), primaryReq->addr, primaryReq); // Use original addr, use copy size

    // --- Cache fill logic removed from here ---

    // Complete all dependent requests: update stats and log, remove from outstandingReqs if needed
    for (CXLRequest* depReq : dependents_copy) { // Use copy
        // Mark as cache miss (since it waited for memory)
        // depReq->cacheHit = false; // REMOVED: Consider dependent requests as hits if merged

        // Complete the dependent request - stats/logging/removal handled inside
        completeRequest(depReq, nullptr); // Pass nullptr as response packet
    }

    // Remove the entry from dependentReqs map after all dependents are processed
    // Use the original iterator depIt, not based on the copy
    dependentReqs.erase(depIt);

    // Check for simulation exit after processing dependents
    // This check is also done in completeRequest, potentially redundant but safe
    if (allRequestsCompleted()) {
        DPRINTF(CXLCard, "All %d requests completed after dependent processing. Exiting simulation.\n", totalRequests);
        exitSimLoop("All CXL requests completed", 0);
    }
}


bool
CXLController::CXLRequestPort::recvTimingResp(PacketPtr pkt)
{
    // Safety check - ensure packet is valid
    if (!pkt) {
        warn("Received null packet in recvTimingResp!");
        return true;
    }

    if (isCache) {
        // Response from Cache
        Addr addr = pkt->getAddr(); // Aligned address

        // Handle cache fill response (WriteResp)
        if (pkt->isResponse() && pkt->cmd == MemCmd::WriteResp) {
            DPRINTF(CXLCard, "Received cache fill response for addr 0x%lx, ignoring.\n", addr);
            delete pkt;
            return true;
        }

        // Find the corresponding request(s) in our tracking map
        auto range = controller->outstandingReqs.equal_range(addr);
        CXLRequest* req = nullptr;

        // Iterate through requests matching the address to find the one matching the packet
        for (auto it = range.first; it != range.second; ++it) {
            // Check if the packet pointer matches the one stored in the request
            if (it->second->pkt == pkt) {
                req = it->second;
                break;
            }
        }

        if (req == nullptr) {
            // Fallback: If pkt pointer doesn't match (e.g., if cache modified it?),
            // try finding *any* non-completed request for this address.
            // This might be less accurate if multiple requests are truly concurrent.
             for (auto it = range.first; it != range.second; ++it) {
                 if (!it->second->completed) {
                     req = it->second;
                     DPRINTF(CXLCard, "Fallback: Found non-completed request %p for cache response addr 0x%lx\n", req, addr);
                     break;
                 }
             }
        }


        if (req == nullptr) {
            warn("Received cache response for unknown or already completed request address: 0x%lx\n", addr);
            delete pkt;
            return true;
        }

        // Check if request was already completed (e.g., as dependent)
        if (req->completed) {
             DPRINTF(CXLCard, "Cache response for already completed request %p (addr 0x%lx), ignoring.\n", req, addr);
             delete pkt;
             return true;
        }


        bool hit = pkt->isResponse() && !pkt->isError();

        DPRINTF(CXLCard, "Response from cache: %s, addr: 0x%lx, hit: %d, error: %d (Req: %p)\n",
                pkt->cmdString(), addr, hit, pkt->isError(), req);

        if (hit) {
            DPRINTF(CXLCard, "Cache %s hit for address 0x%lx (Req: %p)\n",
                   req->isRead ? "read" : "write", addr, req);
            req->cacheHit = true;
            // Complete the request. This will also handle dependents.
            controller->completeRequest(req, pkt);
        } else {
            DPRINTF(CXLCard, "Cache %s miss for address 0x%lx (Req: %p)\n",
                   req->isRead ? "read" : "write", addr, req);
            // Pass the miss packet `pkt` to be deleted inside processCacheMiss
            controller->processCacheMiss(req, pkt);
        }
    } else if (controller->translationPort.name() == name()) {
        // Handle address translation response
        Addr translationAddr = pkt->getAddr();
        DPRINTF(CXLCard, "Received address translation response for addr 0x%lx\n",
                translationAddr);

        Addr origBlockAddr = translationAddr - 0x40000000;

        // Find the matching request(s)
        auto range = controller->outstandingReqs.equal_range(origBlockAddr);
        CXLRequest* req = nullptr;
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second->translationSent && !it->second->translationDone) {
                // Check if the translation packet matches
                if (it->second->transPkt == pkt) {
                    req = it->second;
                    DPRINTF(CXLCard, "Found matching request %p with addr 0x%lx (block addr 0x%lx)\n",
                           req, req->addr, origBlockAddr);
                    break;
                }
            }
        }
         // Fallback if packet pointer doesn't match
        if (!req) {
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second->translationSent && !it->second->translationDone && !it->second->completed) {
                    req = it->second;
                    DPRINTF(CXLCard, "Fallback: Found matching untranslated request %p for block 0x%lx\n", req, origBlockAddr);
                    break;
                }
            }
        }


        if (!req || req->completed) {
            DPRINTF(CXLCard, "Translation response for completed/unknown request (block 0x%lx), ignoring\n", origBlockAddr);
            delete pkt;
            return true;
        }

        req->translationDone = true;
        req->translatedAddr = req->addr; // Simple translation for now
        DPRINTF(CXLCard, "Address 0x%lx translated to 0x%lx (Req %p)\n", req->addr, req->translatedAddr, req);

        // If this was a cache miss waiting for translation, now send to memory
        if (!req->cacheHit && req->sentToCache) {
            controller->sendRequestToMemory(*req);
        }

        // Clean up the translation packet
        delete pkt;
        req->transPkt = nullptr; // Clear pointer in request

    } else { // Memory Port Path (Simplified) -> Response from DecompressionEngine
        Addr addr = pkt->getAddr(); // Aligned translated address
        Addr lineAddr = addr & ~(controller->blockSize - 1);

        DPRINTF(CXLCard, "Received response from decompression engine for addr 0x%lx (pkt %p, cmd %s, size %u)\n",
                addr, pkt, pkt->cmdString(), pkt->getSize());

        // Find the primary request associated with this memory address
        auto memReqIt = controller->outstandingMemReqs.find(lineAddr);
        if (memReqIt == controller->outstandingMemReqs.end()) {
            warn("Received memory response for address 0x%lx, but no matching outstanding memory request found. Deleting pkt %p.\n", lineAddr, pkt);
            // If DecompressionEngine always sends a new packet, we might need to delete it.
            // However, current design is DecompressionEngine reuses CXLController's packet.
            // If pkt is not primaryReq->memPkt, it might need deletion.
            // Let's assume for now it's an error if not found.
            delete pkt;
            return true;
        }

        CXLRequest* primaryReq = memReqIt->second;
        DPRINTF(CXLCard, "Found primary memory request (req %p, orig_addr 0x%lx, memPkt %p) for translated addr 0x%lx (respPkt %p)\n",
               primaryReq, primaryReq->addr, primaryReq->memPkt, lineAddr, pkt);

        // Check if already completed first
        if (primaryReq->completed) {
             DPRINTF(CXLCard, "Primary request %p already completed, ignoring memory response pkt %p.\n", primaryReq, pkt);
             controller->outstandingMemReqs.erase(memReqIt); // Still erase from map
             // If pkt is different from primaryReq->memPkt, it might be a new packet from DecompEngine.
             // If it's the same, CXLRequest destructor will handle it.
             if (pkt != primaryReq->memPkt) {
                 delete pkt;
             }
             return true;
        }

        // The received packet 'pkt' SHOULD BE the same as primaryReq->memPkt
        // if DecompressionEngine reused it and filled data into it.
        if (pkt != primaryReq->memPkt) {
            warn("Memory response packet %p is DIFFERENT from original memPkt %p for req %p. This is unexpected with current design. DecompEngine should return the original memPkt.",
                 pkt, primaryReq->memPkt, primaryReq);
            // If they are different, this indicates a logic error in DecompressionEngine or here.
            // For safety, delete the unexpected 'pkt' and proceed with primaryReq->memPkt if it's valid,
            // or flag an error. For now, assume DecompEngine *must* return primaryReq->memPkt.
            // If DecompEngine created a new packet, it should have copied data to primaryReq->memPkt
            // and this 'pkt' is the new one.
            // This path implies DecompressionEngine sent a *new* packet as response,
            // and did *not* fill data into primaryReq->memPkt.
            // This contradicts the design where DecompressionEngine fills primaryReq->memPkt.
            // If this happens, primaryReq->memPkt might not have the response data.
            // For now, we will assume 'pkt' contains the data and proceed, but this needs fixing.
            // Let's assume DecompressionEngine *did* fill primaryReq->memPkt and pkt is just a wrapper that should be primaryReq->memPkt.
            // If pkt is truly different and contains the data, then primaryReq->memPkt is stale.
            // The design is that DecompressionEngine receives primaryReq->memPkt, fills it, and returns it.
            // So, 'pkt' received here *must* be primaryReq->memPkt.
            // If not, it's a critical error.
            fatal("Memory response packet mismatch: received %p, expected %p for primaryReq %p. DecompressionEngine must return the original memPkt.", pkt, primaryReq->memPkt, primaryReq);
        }

        DPRINTF(CXLCard, "Memory response pkt %p (cmd %s, size %u, hasData %d, isError %d) matches primaryReq->memPkt %p.\n",
                pkt, pkt->cmdString(), pkt->getSize(), pkt->hasData(), pkt->isError(), primaryReq->memPkt);

        // Ensure the packet (primaryReq->memPkt) has data and is valid before using getConstPtr
        if (!primaryReq->memPkt->hasData() && primaryReq->isRead) { // Write responses might not have data
            warn("Primary request's memPkt %p (for read) has no data after DecompressionEngine response for addr 0x%lx. Aborting fill/completion.",
                 primaryReq->memPkt, primaryReq->addr);
            // Potentially an error in DecompressionEngine not setting data.
            // We cannot proceed to copy data if it's not there.
            controller->outstandingMemReqs.erase(memReqIt); // Clean up map
            // Don't complete the request as it's errored. Or complete with error.
            // For now, just return. This will likely lead to a hang or timeout.
            return true;
        }


        // Remove from outstanding memory requests map *before* potential cache fill send
        controller->outstandingMemReqs.erase(memReqIt);

        // --- Cache Fill Logic ---
        // If this was a read miss, send a cache fill request
        if (primaryReq->isRead && !primaryReq->cacheHit) {
            Addr cacheLineAddr = primaryReq->addr & ~(controller->blockSize - 1);
            auto fillReq_s = std::make_shared<Request>(cacheLineAddr, controller->blockSize, 0, 0); // Renamed to avoid conflict
            PacketPtr fillPkt = new Packet(fillReq_s, MemCmd::WriteLineReq);
            fillPkt->allocate();

            // Copy data from the memory response packet (primaryReq->memPkt, which is 'pkt')
            // This is where the assertion `flags.isSet(STATIC_DATA|DYNAMIC_DATA)` happens.
            // We must ensure primaryReq->memPkt (which is 'pkt') is valid.
            DPRINTF(CXLCard, "Attempting to copy data for cache fill from primaryReq->memPkt %p (size %u, hasData %d)\n",
                    primaryReq->memPkt, primaryReq->memPkt->getSize(), primaryReq->memPkt->hasData());

            if (primaryReq->memPkt->hasData()) { // Use primaryReq->memPkt directly
                size_t copySize = std::min((size_t)primaryReq->memPkt->getSize(), (size_t)fillPkt->getSize());
                std::memcpy(fillPkt->getPtr<uint8_t>(),
                           primaryReq->memPkt->getConstPtr<uint8_t>(), // Use primaryReq->memPkt
                           copySize);
                DPRINTF(CXLCard, "Copied %lu bytes from mem response pkt to fillPkt for primary req %p\n", (unsigned long)copySize, primaryReq); // MODIFIED: format specifier and cast
            } else {
                 warn("Memory response pkt (primaryReq->memPkt %p) has no data for cache fill in recvTimingResp (req %p)", primaryReq->memPkt, primaryReq);
            }

            // Try to send the cache fill request
            bool success = controller->cachePort.sendTimingReq(fillPkt);
            if (!success) {
                DPRINTF(CXLCard, "Cache fill request failed for primary req %p, adding to retry queue\n", primaryReq);
                controller->cacheRetryQueue.push(fillPkt);
            } else {
                DPRINTF(CXLCard, "Successfully sent cache fill request for primary req %p\n", primaryReq);
            }
        }
        // --- End Cache Fill Logic ---

        // Mark primary as cache miss
        primaryReq->cacheHit = false;

        // Complete the primary request. This handles dependents and removal from outstandingReqs.
        // Pass primaryReq->memPkt as the response packet.
        // The data for the original request is now in primaryReq->memPkt.
        controller->completeRequest(primaryReq, primaryReq->memPkt); // Pass memPkt

        // DO NOT delete pkt (which is primaryReq->memPkt) here.
        // It will be deleted by CXLRequest's destructor if completeRequest doesn't take ownership,
        // or handled by completeRequest if it does.
        // Current completeRequest logic expects to delete respPkt if it's not memPkt.
        // Since we are passing memPkt, it should not be deleted there.
    }

    return true;
}

void
CXLController::CXLRequestPort::recvReqRetry()
{
    // Retry sending packets to the appropriate destination based on port name
    if (name() == controller->cachePort.name()) {
        controller->trySendRetries(true); // Retry cache queue
    } else if (name() == controller->memPort.name()) {
        controller->trySendRetries(false); // Retry memory queue
    } else if (name() == controller->translationPort.name()) {
        controller->trySendTranslationRetries(); // Retry translation queue
    } else {
        panic("Unknown port received retry: %s", name());
    }
}

void
CXLController::trySendRetries(bool toCache)
{
    // This function now only handles cacheRetryQueue and memRetryQueue
    auto &retryQueue = toCache ? cacheRetryQueue : memRetryQueue;
    auto &port = toCache ? cachePort : memPort;

    // Try to send packets from the retry queue
    while (!retryQueue.empty()) {
        PacketPtr pkt = retryQueue.front();

        // Safety check for null packet
        if (!pkt) {
            warn("Null packet in %s retry queue!", toCache ? "cache" : "memory");
            retryQueue.pop();
            continue;
        }

        // Only try to access packet contents if it's valid
        DPRINTF(CXLCard, "Attempting to retry packet for addr 0x%lx to %s\n",
               pkt->getAddr(), toCache ? "cache" : "memory");

        if (!port.sendTimingReq(pkt)) {
            // Still blocked, will retry later
            DPRINTF(CXLCard, "Retry sending packet for addr 0x%lx to %s still blocked\n",
                   pkt->getAddr(), toCache ? "cache" : "memory");
            return;
        }

        DPRINTF(CXLCard, "Successfully resent packet for addr 0x%lx to %s\n",
               pkt->getAddr(), toCache ? "cache" : "memory");
        retryQueue.pop();
    }
}

// ADDED function to handle translation retries
void
CXLController::trySendTranslationRetries()
{
    // Try to send packets from the translation retry queue
    while (!translationRetryQueue.empty()) {
        PacketPtr pkt = translationRetryQueue.front();

        // Safety check for null packet
        if (!pkt) {
            warn("Null packet in translation retry queue!");
            translationRetryQueue.pop();
            continue;
        }

        DPRINTF(CXLCard, "Attempting to retry translation packet for addr 0x%lx\n",
               pkt->getAddr());

        if (!translationPort.sendTimingReq(pkt)) {
            // Still blocked, will retry later
            DPRINTF(CXLCard, "Retry sending translation packet for addr 0x%lx still blocked\n",
                   pkt->getAddr());
            return;
        }

        DPRINTF(CXLCard, "Successfully resent translation packet for addr 0x%lx\n",
               pkt->getAddr());
        translationRetryQueue.pop();
    }
}

bool
CXLController::allRequestsCompleted() const
{
    return completedRequests == totalRequests && totalRequests > 0;
}

void
CXLController::completeRequest(CXLRequest* req, PacketPtr respPkt)
{
    // Check if this specific request instance has already been completed
    if (req->completed) {
        DPRINTF(CXLCard, "Request %p for addr 0x%lx already completed, skipping completeRequest.\n", req, req->addr);
        // Clean up the optional response packet if provided AND it's not the request's own memPkt
        if (respPkt && respPkt != req->memPkt && respPkt != req->pkt && respPkt != req->transPkt) {
             delete respPkt;
        }
        return;
    }

    // Mark as completed FIRST, before processing dependents, to avoid loops
    req->completed = true;

    // Calculate request latency in ticks using arrivalTick
    Tick latency_ticks = 0;
    if (req->arrivalTick > 0) { // Ensure arrivalTick was set
        latency_ticks = curTick() - req->arrivalTick;
    } else {
        warn("Request %p completed with arrivalTick=0, latency calculation might be inaccurate.", req);
        // Fallback to sendTick if arrivalTick is missing? Or just 0?
        if (req->sendTick > 0) {
             latency_ticks = curTick() - req->sendTick;
        } else {
             latency_ticks = 0; // Or handle differently
        }
    }

    double latency_ns = static_cast<double>(latency_ticks) / 1000.0;

    // Determine hit/miss status (req->cacheHit should be set correctly by caller or dependent logic)
    bool isHit = req->cacheHit;
    bool isRead = req->isRead;

    // --- Logging to File ---
    if (outputFile.is_open()) {
        // Use manipulators for fixed-width output
        // Add req->time_us for ArrivalTime(us)
        outputFile << std::left << "0x" << std::hex << std::setw(16) << req->addr << std::dec // Address (18 width total)
                   << std::fixed << std::setprecision(3) << std::setw(15) << req->time_us // Arrival Time (us) // ADDED
                   << std::fixed << std::setprecision(2) << std::setw(22) << req->comprRatio // Compression Ratio
                   << std::setw(15) << latency_ns // Latency
                   << std::setw(8) << (isHit ? "1" : "0") // IsHit
                   << std::endl;
    }

    // --- Update Summary Statistics ---
    totalLatencySum += latency_ns;
    if (isHit) {
        hitLatencySum += latency_ns;
        hitCount++;
    } else {
        missLatencySum += latency_ns;
        missCount++;
    }

    // --- Update Statistics Counters ---
    stats.totalRequests++; // Increment completed requests counter stat

    if (isRead) {
        // Removed readLatency stat update
        if (isHit) {
            // Removed hitLatency and readHitLatency stat updates
            stats.totalHits++;
            stats.readHits++;
        } else {
            // Removed missLatency and readMissLatency stat updates
            stats.totalMisses++;
            stats.readMisses++;
        }
    } else { // Write
        // Removed writeLatency stat update
        if (isHit) {
            // Removed hitLatency and writeHitLatency stat updates
            stats.totalHits++;
            stats.writeHits++;
        } else {
            // Removed missLatency and writeMissLatency stat updates
            stats.totalMisses++;
            stats.writeMisses++;
        }
    }

    // Print completion information with floating point time (optional, kept for debug)
    DPRINTF(CXLCard, "Completed CXL Request: %s Address: 0x%lx Time: %.3f us Compression Ratio: %.2f Latency: %.2f ns (%s) (Req: %p)\n",
           req->isRead ? "Read" : "Write",
           req->addr, // Use original address for logging
           req->time_us,
           req->comprRatio,
           latency_ns, // Log the same value being added to stats
           isHit ? "cache hit" : "cache miss",
           req);

    // Clean up the optional response packet passed for cache hits etc.
    // The primary memory response packet (req->memPkt) is now passed as respPkt for memory path.
    // It should NOT be deleted here if it's one of the request's own packets.
    // CXLRequest destructor will handle req->pkt, req->memPkt, req->transPkt.
    if (respPkt && respPkt != req->memPkt && respPkt != req->pkt && respPkt != req->transPkt) {
         DPRINTF(CXLCard, "Deleting respPkt %p in completeRequest as it's not one of req %p's main packets.\n", respPkt, req);
         delete respPkt;
    } else if (respPkt) {
         DPRINTF(CXLCard, "Not deleting respPkt %p in completeRequest as it is one of req %p's main packets (pkt: %p, memPkt: %p, transPkt: %p).\n",
                 respPkt, req, req->pkt, req->memPkt, req->transPkt);
    }

    // Remove this specific request instance from outstanding requests map
    Addr lineAddr = req->addr & ~(blockSize - 1);
    auto range = outstandingReqs.equal_range(lineAddr);
    bool removed = false;

    // Use a more robust loop for erasing from multimap
    for (auto it = range.first; it != range.second; ) { // Remove ++it here
        if (it->second == req) {
            it = outstandingReqs.erase(it); // Erase and update iterator
            removed = true;
            DPRINTF(CXLCard, "Removed request %p from outstandingReqs for addr 0x%lx\n", req, lineAddr);
            // Break assuming only one instance of the exact req pointer exists
            break;
        } else {
            ++it; // Increment only if not erased
        }
    }

    if (!removed) {
         // This might happen if a dependent request is completed before the primary? Should not happen with new logic.
         // Or if called multiple times for the same request.
         DPRINTF(CXLCard, "Could not find request %p in outstandingReqs to remove for addr 0x%lx (maybe already removed or pointer mismatch?)\n", req, lineAddr); // MODIFIED message
    }

    // --- Complete Dependent Requests ---
    // If this was a primary request, complete its dependents
    if (!req->isWaitingForMemory) {
        completeDependentRequests(req);
    }

    // Increment completed requests counter (internal tracking)
    completedRequests++;

    // If all requests are completed, exit the simulation
    // Compare completedRequests with totalRequests loaded from trace
    if (allRequestsCompleted()) { // Use the helper function
        DPRINTF(CXLCard, "All %d requests completed. Exiting simulation.\n", totalRequests);
        // Exit simulation normally, which will trigger the exit callback
        exitSimLoop("All CXL requests completed", 0);
    }
}

void
CXLController::processCacheMiss(CXLRequest* req, PacketPtr missPkt)
{
    // Mark as cache miss (should already be done, but ensure)
    req->cacheHit = false;

    Addr blockAddr = req->addr & ~(blockSize - 1);
    DPRINTF(CXLCard, "Cache miss for block address 0x%lx (request addr 0x%lx, Req: %p), checking translation\n",
            blockAddr, req->addr, req);

    // Clean up the cache miss packet
    delete missPkt;
    // Ensure the request's packet pointer is cleared if it pointed to missPkt
    if (req->pkt == missPkt) {
        req->pkt = nullptr;
    }


    // Only send read misses to memory (write misses are ignored with no-write-allocate)
    if (req->isRead) {
        // Check if a memory request for this CXLRequest has already been sent
        // This means req->memPkt would be non-null if sendRequestToMemory was called previously.
        if (req->memPkt != nullptr) {
            DPRINTF(CXLCard, "Memory request already in progress or queued for Req %p (memPkt %p), not sending again from processCacheMiss.\n", req, req->memPkt);
            return; // Do not send another memory request if one is already pending/sent
        }

        // Check if translation is already done
        if (req->translationDone) {
            DPRINTF(CXLCard, "Translation already complete for Req %p, proceeding with memory request\n", req);
            sendRequestToMemory(*req);
        } else {
            DPRINTF(CXLCard, "Req %p waiting for address translation to complete\n", req);
            // The translation response handler will call sendRequestToMemory
        }
    } else {
        // For write misses with no-write-allocate, we should have already completed the request?
        // Or should we send to memory? Current logic assumes writes don't go to memory on miss.
        // If writes *should* go to memory on miss, add similar logic as for reads.
        // For now, assume write misses are completed immediately (no-write-allocate).
        // If this path is reached for a write, it implies an issue.
        warn("Unexpected write miss processing in processCacheMiss for Req %p, addr 0x%lx\n", req, req->addr);
        // Complete the request immediately as a miss if it wasn't already
        if (!req->completed) {
             completeRequest(req, nullptr);
        }
    }
}

bool
CXLController::sendRequestToCache(CXLRequest &req)
{
    // Calculate the cache line address
    Addr lineAddr = req.addr & ~(blockSize - 1);

    // Create the request
    auto memReq = std::make_shared<Request>(
        lineAddr, blockSize, 0, 0);

    // Create the packet
    PacketPtr pkt = new Packet(memReq, req.isRead ?
                              MemCmd::ReadReq : MemCmd::WriteReq);

    // Set packet size and allocate memory if needed
    pkt->allocate();

    // If it's a write request, fill with some data
    if (!req.isRead) {
        std::memset(pkt->getPtr<uint8_t>(), 0xA5, blockSize);
    }

    // Store the packet in the request
    req.pkt = pkt;
    req.sentToCache = true;
    req.cacheHit = false; // Default to false until we confirm hit

    // Record when the request is sent
    req.sendTick = curTick();

    // Add to outstanding requests map using insert for multimap
    // Find the original request pointer from the requests vector
    CXLRequest* trackedReq = nullptr;
    // This linear search is inefficient but necessary if req is a copy
    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].addr == req.addr && requests[i].time_us == req.time_us && !requests[i].completed) {
             // Basic matching, might need better ID if traces have identical reqs
             if (!trackedReq) trackedReq = &requests[i];
             // If multiple identical requests exist, this picks the first non-completed one
        }
    }

    if (!trackedReq) {
         // If not found in original vector (e.g., if req was dynamically created?),
         // we might need to store it differently. For now, assume it's from the vector.
         warn("Could not find original request in vector for request to addr 0x%lx time %.3f", req.addr, req.time_us);
         // Fallback: use the address of the passed-in req, but this might be temporary
         trackedReq = &req;
         // This could lead to issues if 'req' is on the stack.
         // A better approach might be needed if requests aren't always from the initial vector.
    }


    if (trackedReq) {
        outstandingReqs.insert({lineAddr, trackedReq});
        DPRINTF(CXLCard, "Added request %p to outstandingReqs for addr 0x%lx\n", trackedReq, lineAddr);
    } else {
         // Handle error: couldn't track the request
         warn("Failed to track request for cache send: Addr 0x%lx", req.addr);
         delete pkt; // Clean up packet
         req.pkt = nullptr;
         return false; // Indicate failure
    }


    // Print request information with floating point time
    DPRINTF(CXLCard, "Sending CXL Request to Cache: %s Address: 0x%lx Time: %.3f us Compression Ratio: %.2f (Req: %p)\n",
           req.isRead ? "Read" : "Write",
           req.addr,
           req.time_us,
           req.comprRatio,
           trackedReq); // Log the tracked pointer

    // Send the packet to cache
    bool success = cachePort.sendTimingReq(pkt);
    if (!success) {
        // If sending fails immediately, add to retry queue
        DPRINTF(CXLCard, "Cache port busy, adding pkt %p to retry queue for Req %p\n", pkt, trackedReq);
        cacheRetryQueue.push(pkt);
        // Return true because we accepted the request (it's queued)
        return true;
    }
    return success; // Should be true if not added to retry queue
}

bool
CXLController::sendRequestToMemory(CXLRequest &req)
{
    // Always use the translated address
    assert(req.translationDone && "Translation must be complete before sending to memory");

    // Calculate the aligned address for memory access
    Addr lineAddr = req.translatedAddr & ~(blockSize - 1);
    DPRINTF(CXLCard, "Using translated address 0x%lx for memory request (Req %p)\n", lineAddr, &req);

    // --- Merging logic removed from here ---
    // Check if there's already a pending request for this address
    // auto existingIt = outstandingMemReqs.find(lineAddr);
    // if (existingIt != outstandingMemReqs.end()) { ... }

    // Calculate the raw compressed size based on compression ratio
    // comprRatio is in percentage, e.g. 50.0 means 50% of original size
    unsigned raw_compressed_size = std::max(static_cast<unsigned>(
        static_cast<double>(blockSize) * req.comprRatio / 100.0), 1u);

    // Round up to the nearest multiple of cacheLineSize.
    // cacheLineSize is a SimObject parameter, expected to be > 0.
    assert(cacheLineSize > 0 && "cacheLineSize must be positive for rounding.");
    unsigned compressedSize = ((raw_compressed_size + cacheLineSize - 1) / cacheLineSize) * cacheLineSize;

    // If raw_compressed_size was 0 (prevented by std::max), compressedSize could be 0.
    // But since raw_compressed_size >= 1 and cacheLineSize >= 1,
    // compressedSize will be at least cacheLineSize.
    // e.g. raw_compressed_size=1, cacheLineSize=64 -> compressedSize=64.

    DPRINTF(CXLCard, "Original size: %u bytes, Raw compressed size: %u bytes, Final rounded compressed size: %u bytes (Ratio: %.2f%%, CacheLineSize: %u)\n",
            blockSize, raw_compressed_size, compressedSize, req.comprRatio, cacheLineSize);

    // Create the request for memory with the compressed size
    auto memReq = std::make_shared<Request>(
        lineAddr, compressedSize, 0, 0);

    // Create the packet for direct memory access
    PacketPtr pkt = new Packet(memReq, req.isRead ?
                              MemCmd::ReadReq : MemCmd::WriteReq);

    // Set packet size and allocate memory if needed
    pkt->allocate();

    // If it's a write request, fill with some data
    if (!req.isRead) {
        std::memset(pkt->getPtr<uint8_t>(), 0xA5, compressedSize);
    }

    // Include compression ratio in the packet data for decompression engine
    // Store in first 8 bytes of the packet data (simple approach)
    if (pkt->hasData() && pkt->getSize() >= sizeof(double)) {
        *reinterpret_cast<double*>(pkt->getPtr<uint8_t>()) = req.comprRatio;
    } else if (pkt->hasData()) {
         warn("Packet data size (%u) too small to store compression ratio (needs %lu)", pkt->getSize(), sizeof(double));
    }


    // Store the memory packet in the request
    // Ensure we don't overwrite an existing packet pointer if this function is called multiple times for the same request (shouldn't happen)
    if (req.memPkt) {
        warn("Req %p already has a memPkt %p assigned when creating new memPkt %p", &req, req.memPkt, pkt);
        delete req.memPkt; // Delete old packet to prevent leak
    }
    req.memPkt = pkt;


    // Print request information with addresses and compression ratio
    DPRINTF(CXLCard, "Sending CXL Request to Memory: %s Address: 0x%lx (orig: 0x%lx) Size: %u bytes, Ratio: %.2f%% (Req: %p)\n",
           req.isRead ? "Read" : "Write",
           lineAddr, req.addr, compressedSize, req.comprRatio, &req);

    // Register this as an outstanding memory request
    // Ensure we use the correct pointer to the request object
    outstandingMemReqs[lineAddr] = &req;

    // Send the packet directly to memory
    bool success = memPort.sendTimingReq(pkt);
     if (!success) {
        // If sending fails immediately, add to retry queue
        DPRINTF(CXLCard, "Memory port busy, adding memPkt %p to retry queue for Req %p\n", pkt, &req);
        memRetryQueue.push(pkt);
        // Return true because we accepted the request (it's queued)
        return true;
    }
    return success; // Should be true if not added to retry queue
}


bool
CXLController::sendAddressTranslationRequest(CXLRequest &req)
{
    // Get the original address
    Addr origAddr = req.addr;

    // Get the block address (aligned to cache line size)
    Addr blockAddr = origAddr & ~(blockSize - 1);

    // Translation table is in the second half of memory (0x40000000 - 0x80000000)
    // Calculate a lookup address in the translation table based on the block address
    Addr translationAddr = 0x40000000 + blockAddr;

    DPRINTF(CXLCard, "Translation lookup: original addr 0x%lx (block addr 0x%lx) → table lookup addr 0x%lx (Req: %p)\n",
            origAddr, blockAddr, translationAddr, &req);

    // Create request and packet - explicitly use 8 bytes (64 bits) for translation lookup
    auto transReq = std::make_shared<Request>(translationAddr, 8, 0, 0);
    PacketPtr pkt = new Packet(transReq, MemCmd::ReadReq);
    pkt->allocate();

    // Store in request
    // Ensure we don't overwrite an existing packet pointer
    if (req.transPkt) {
        warn("Req %p already has a transPkt %p assigned when creating new transPkt %p", &req, req.transPkt, pkt);
        delete req.transPkt; // Delete old packet
    }
    req.transPkt = pkt;
    req.translationSent = true;


    // Send request
    bool success = translationPort.sendTimingReq(pkt);
    if (!success) {
        DPRINTF(CXLCard, "Translation port busy, adding transPkt %p to retry queue for Req %p\n", pkt, &req); // MODIFIED message
        translationRetryQueue.push(pkt);
        // Return true because we accepted the request (it's queued)
        return true;
    }

    return success; // Should be true if not added to retry queue
}

void
CXLController::processRequest(const CXLRequest &reqEvent)
{
    // Find the request in our vector to get a stable pointer
    CXLRequest* trackedReq = nullptr;
    for (size_t i = 0; i < requests.size(); i++) {
        // Match based on address and time, and ensure it's not already completed
        if (requests[i].addr == reqEvent.addr &&
            requests[i].time_us == reqEvent.time_us &&
            !requests[i].completed)
        {
            // If multiple identical requests exist, this picks the first non-completed one
            trackedReq = &requests[i];
            break;
        }
    }

    if (trackedReq == nullptr) {
        warn("Could not find matching non-completed request in request vector for addr 0x%lx time %.3f, skipping",
             reqEvent.addr, reqEvent.time_us);
        return;
    }

    // Record arrival time when the request is first processed
    if (trackedReq->arrivalTick == 0) { // Only set it once
        trackedReq->arrivalTick = curTick();
    }

    DPRINTF(CXLCard, "Processing request %p: Addr 0x%lx Time %.3f\n", trackedReq, trackedReq->addr, trackedReq->time_us);

    // --- Early Merging Logic ---
    Addr blockAddr = trackedReq->addr & ~(blockSize - 1);
    CXLRequest* primaryReq = nullptr;

    // Check if any request for this block is already outstanding
    auto range = outstandingReqs.equal_range(blockAddr);
    for (auto it = range.first; it != range.second; ++it) {
        // Find the primary request (the one that isn't waiting for another)
        // AND ensure it's not already completed (important addition)
        if (!it->second->isWaitingForMemory && !it->second->completed) { // ADDED check for !completed
            primaryReq = it->second;
            break;
        }
    }

    // Add the current request to outstandingReqs regardless
    // Check if it's already there before inserting (to avoid duplicates if processRequest is called multiple times for the same event)
    bool already_outstanding = false;
    // Re-iterate range to check if trackedReq is already present
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == trackedReq) {
            already_outstanding = true;
            break;
        }
    }
    if (!already_outstanding) {
        outstandingReqs.insert({blockAddr, trackedReq});
        DPRINTF(CXLCard, "Added request %p to outstandingReqs for addr 0x%lx\n", trackedReq, blockAddr);
    }


    if (primaryReq != nullptr && primaryReq != trackedReq) {
        // Found an existing primary request for this block, merge the new one
        DPRINTF(CXLCard, "Found existing primary request %p for block 0x%lx, merging current Req %p\n",
               primaryReq, blockAddr, trackedReq);

        // Mark this request as waiting
        trackedReq->isWaitingForMemory = true;
        trackedReq->waitingForRequest = primaryReq;
        trackedReq->cacheHit = true; // ADDED: Mark merged request as a hit
        // Set sendTick to 0 or primary's sendTick? Let's use 0 for now.
        trackedReq->sendTick = 0; // sendTick is still used to track when sent to cache/mem

        // Add this request to the dependent requests list of the primary
        dependentReqs[primaryReq].push_back(trackedReq);

        DPRINTF(CXLCard, "Added dependent request %p to primary request %p - now %u dependent requests\n",
              trackedReq, primaryReq, dependentReqs[primaryReq].size());

        // Do not proceed with cache/translation lookup for the merged request
        return;
    } else if (primaryReq == trackedReq) {
         // This case should ideally not happen if trackedReq was just added,
         // unless it somehow got added before and we are processing it again?
         warn("processRequest called for request %p which seems to be already the primary outstanding request.", trackedReq);
         // Proceed as if it's a new primary request anyway.
    }


    // --- End Early Merging Logic ---

    // If we reached here, this is a new primary request for this block
    DPRINTF(CXLCard, "Request %p is primary for block 0x%lx\n", trackedReq, blockAddr);

    // Send address translation request in parallel with cache request
    sendAddressTranslationRequest(*trackedReq);

    // First try the cache
    // sendRequestToCache sets the sendTick
    sendRequestToCache(*trackedReq);
    // sendRequestToCache now handles adding to retry queue internally if needed
}

void
CXLController::startup()
{
    // Load the trace file
    loadTrace();

    // Schedule all events
    scheduleEvents();
}

void
CXLController::loadTrace()
{
    std::ifstream traceFile(traceFilePath);
    if (!traceFile.is_open()) {
        fatal("Could not open CXL trace file: %s", traceFilePath);
    }

    std::string line;
    while (std::getline(traceFile, line)) {
        std::istringstream iss(line);
        char rw;
        Addr addr;
        double time_us;  // Changed from uint64_t to double
        double comprRatio;

        if (!(iss >> rw >> std::hex >> addr >> std::dec >> time_us >> comprRatio)) {
            warn("Ignoring malformed line in trace file: %s", line);
            continue;
        }

        CXLRequest req;
        req.isRead = (rw == 'R');
        req.addr = addr;
        req.time_us = time_us;
        req.comprRatio = comprRatio;

        requests.push_back(req);
    }

    // Update the total number of requests
    totalRequests = requests.size();

    // Fix the format specifier for size_t
    DPRINTF(CXLCard, "Loaded %u CXL requests from trace file: %s\n",
           requests.size(), traceFilePath);
}

void
CXLController::scheduleEvents()
{
    int eventId = 0;
    for (auto& req : requests) {
        // Create and schedule event
        auto event = new CXLRequestEvent(req,
            csprintf("%s-event-%d", name(), eventId++), this);

        // Convert microseconds to ticks (1 us = 1000 ps)
        // This assumes the simulation tick is 1 ps
        Tick tick_time = req.time_us * gem5::sim_clock::as_float::us;

        schedule(event, tick_time);
        DPRINTF(CXLCard, "Scheduled CXL request event at %lu ticks\n", tick_time);
    }
}

Port &
CXLController::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cache_port") {
        return cachePort;
    } else if (if_name == "mem_port") {
        return memPort;
    } else if (if_name == "translation_port") {
        return translationPort;
    } else {
        return SimObject::getPort(if_name, idx);
    }
}

} // namespace gem5
