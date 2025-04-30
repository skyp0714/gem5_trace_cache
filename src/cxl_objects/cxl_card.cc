#include "cxl_objects/cxl_card.hh"

#include <fstream>
#include <string>
#include <sstream>

#include "base/logging.hh"
#include "sim/core.hh"
#include "debug/CXLCard.hh"
#include "sim/sim_exit.hh"
#include "mem/packet_access.hh"

namespace gem5
{

// Define the stats group constructor
CXLController::CXLStats::CXLStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(meanAccessLatency, "Mean access latency in nanoseconds"),
      ADD_STAT(readLatency, "Mean read access latency in nanoseconds"),
      ADD_STAT(writeLatency, "Mean write access latency in nanoseconds"),
      ADD_STAT(hitLatency, "Mean access latency for cache hits in nanoseconds"),
      ADD_STAT(missLatency, "Mean access latency for cache misses in nanoseconds"),
      ADD_STAT(readHitLatency, "Mean access latency for read hits in nanoseconds"),
      ADD_STAT(readMissLatency, "Mean access latency for read misses in nanoseconds"),
      ADD_STAT(writeHitLatency, "Mean access latency for write hits in nanoseconds"),
      ADD_STAT(writeMissLatency, "Mean access latency for write misses in nanoseconds"),
      ADD_STAT(totalRequests, "Total number of requests"),
      ADD_STAT(totalHits, "Total number of cache hits"),
      ADD_STAT(totalMisses, "Total number of cache misses"),
      ADD_STAT(readHits, "Number of read hits"),
      ADD_STAT(readMisses, "Number of read misses"),
      ADD_STAT(writeHits, "Number of write hits"),
      ADD_STAT(writeMisses, "Number of write misses"),
      ADD_STAT(hitRate, "Cache hit rate")
{
    hitRate.name("hitRate");
    hitRate.desc("Cache hit rate (hits/total)");
    hitRate = totalHits / totalRequests;
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
      cacheLineSize(p.cache_line_size),
      cachePort(name() + ".cache_port", this, true),
      memPort(name() + ".mem_port", this, false),
      translationPort(name() + ".translation_port", this, false),
      totalRequests(0),
      completedRequests(0),
      stats(this)  // Fix initialization order to match declaration order
{
}


CXLController::~CXLController()
{
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
CXLController::completeDependentRequests(CXLRequest* primaryReq) // Renamed parameter
{
    auto depIt = dependentReqs.find(primaryReq);
    if (depIt == dependentReqs.end()) {
        DPRINTF(CXLCard, "No dependent requests for primary addr 0x%lx (req %p)\n",
               primaryReq->translatedAddr & ~(cacheLineSize - 1), primaryReq);
        return;
    }

    // Get the list of dependent requests
    auto& dependents = depIt->second;

    DPRINTF(CXLCard, "Completing %u dependent requests for primary addr 0x%lx (req %p)\n",
            dependents.size(), primaryReq->translatedAddr & ~(cacheLineSize - 1), primaryReq);

    // --- Cache fill logic removed from here ---

    // Complete all dependent requests: update stats and log, remove from outstandingReqs if needed
    for (CXLRequest* depReq : dependents) {
        // Mark as cache miss (since it waited for memory)
        depReq->cacheHit = false;

        // Complete the dependent request - stats/logging/removal handled inside
        DPRINTF(CXLCard, "Completing dependent request %p (addr 0x%lx)\n", depReq, depReq->addr);
        completeRequest(depReq, nullptr); // Pass nullptr as response packet
    }

    // Remove the entry from dependentReqs map after all dependents are processed
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
            // Pass the actual response packet `pkt`
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

    } else { // Memory Port Path
        Addr addr = pkt->getAddr(); // Aligned translated address
        Addr lineAddr = addr & ~(controller->cacheLineSize - 1);

        DPRINTF(CXLCard, "Received response from decompression engine for addr 0x%lx\n", addr);

        // Find the primary request associated with this memory address
        auto memReqIt = controller->outstandingMemReqs.find(lineAddr);
        if (memReqIt == controller->outstandingMemReqs.end()) {
            warn("Received memory response for address 0x%lx, but no matching outstanding memory request found.\n", lineAddr);
            delete pkt;
            return true;
        }

        CXLRequest* primaryReq = memReqIt->second;
        DPRINTF(CXLCard, "Found primary memory request (req %p, orig_addr 0x%lx) for translated addr 0x%lx\n",
               primaryReq, primaryReq->addr, lineAddr);

        // Store the memory response packet in the primary request *before* erasing from outstandingMemReqs
        if (!primaryReq->memPkt) {
             primaryReq->memPkt = pkt;
        } else if (primaryReq->memPkt != pkt) {
             warn("Primary request %p already had a different memPkt %p assigned when receiving response %p",
                  primaryReq, primaryReq->memPkt, pkt);
             delete pkt;
             return true;
        }

        controller->outstandingMemReqs.erase(memReqIt);

        // Check if the primary request was already completed
        if (primaryReq->completed) {
             DPRINTF(CXLCard, "Primary request %p already completed, ignoring memory response.\n", primaryReq);
             delete primaryReq->memPkt;
             primaryReq->memPkt = nullptr;
             return true;
        }

        // --- Cache Fill Logic Moved Here ---
        // If this was a read miss, send a cache fill request
        if (primaryReq->isRead && !primaryReq->cacheHit) {
            Addr cacheLineAddr = primaryReq->addr & ~(controller->cacheLineSize - 1);
            auto fillReq = std::make_shared<Request>(cacheLineAddr, controller->cacheLineSize, 0, 0);
            PacketPtr fillPkt = new Packet(fillReq, MemCmd::WriteLineReq);
            fillPkt->allocate();

            // Copy data from the memory response packet
            if (primaryReq->memPkt && primaryReq->memPkt->hasData()) {
                size_t copySize = std::min((size_t)primaryReq->memPkt->getSize(), (size_t)fillPkt->getSize());
                std::memcpy(fillPkt->getPtr<uint8_t>(),
                           primaryReq->memPkt->getConstPtr<uint8_t>(),
                           copySize);
                DPRINTF(CXLCard, "Copied %u bytes from memPkt to fillPkt for primary req %p\n", copySize, primaryReq);
            } else {
                 warn("Primary request memPkt (req %p) has no data or is null for cache fill in recvTimingResp", primaryReq);
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


        // Process any dependent requests that were waiting for this response
        controller->completeDependentRequests(primaryReq);

        // Now, complete the primary request itself
        if (primaryReq->completed) {
             DPRINTF(CXLCard, "Primary request %p completed during dependent processing, ignoring memory response for primary.\n", primaryReq);
             delete primaryReq->memPkt;
             primaryReq->memPkt = nullptr;
             return true;
        }

        // Mark primary as cache miss (redundant if already false, but safe)
        primaryReq->cacheHit = false;

        // Complete the primary request
        controller->completeRequest(primaryReq, nullptr);

        // Clean up the original response packet
        delete primaryReq->memPkt;
        primaryReq->memPkt = nullptr;
    }

    return true;
}

void
CXLController::CXLRequestPort::recvReqRetry()
{
    // Retry sending packets to the appropriate destination
    controller->trySendRetries(isCache);
}

void
CXLController::trySendRetries(bool toCache)
{
    auto &retryQueue = toCache ? cacheRetryQueue : memRetryQueue;
    auto &port = toCache ? cachePort : memPort;

    // Try to send packets from the retry queue
    while (!retryQueue.empty()) {
        PacketPtr pkt = retryQueue.front();

        // Safety check for null packet
        if (!pkt) {
            warn("Null packet in retry queue!");
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
        // Clean up the optional response packet if provided
        if (respPkt) delete respPkt;
        return;
    }

    // Mark as completed
    req->completed = true;

    // Calculate request latency in ticks and convert to nanoseconds
    Tick latency_ticks = curTick() - req->sendTick;
    double latency_ns = static_cast<double>(latency_ticks) / 1000.0;

    // Determine hit/miss status (req->cacheHit should be set correctly by caller)
    bool isHit = req->cacheHit;
    bool isRead = req->isRead;

    // Record latency in the appropriate stats
    stats.meanAccessLatency = latency_ns;
    // Increment total requests count - Use stats::Scalar type
    stats.totalRequests++;

    if (isRead) {
        stats.readLatency = latency_ns;
        if (isHit) {
            stats.hitLatency += latency_ns;
            stats.readHitLatency += latency_ns;
            stats.totalHits++;
            stats.readHits++;
        } else {
            stats.missLatency += latency_ns;
            stats.readMissLatency += latency_ns;
            stats.totalMisses++;
            stats.readMisses++;
        }
    } else { // Write
        stats.writeLatency += latency_ns;
        if (isHit) {
            stats.hitLatency += latency_ns;
            stats.writeHitLatency += latency_ns;
            stats.totalHits++;
            stats.writeHits++;
        } else {
            stats.missLatency += latency_ns;
            stats.writeMissLatency += latency_ns;
            stats.totalMisses++;
            stats.writeMisses++;
        }
    }

    // Print completion information with floating point time
    DPRINTF(CXLCard, "Completed CXL Request: %s Address: 0x%lx Time: %.3f us Compression Ratio: %.2f Latency: %.2f ns (%s) (Req: %p)\n",
           req->isRead ? "Read" : "Write",
           req->addr, // Use original address for logging
           req->time_us,
           req->comprRatio,
           latency_ns,
           isHit ? "cache hit" : "cache miss",
           req);

    // Clean up the optional response packet passed for cache hits etc.
    // The primary memory response packet (req->memPkt) is deleted by the caller (recvTimingResp memory path)
    if (respPkt && respPkt != req->memPkt) {
         delete respPkt;
    }

    // Remove this specific request instance from outstanding requests map
    Addr lineAddr = req->addr & ~(cacheLineSize - 1);
    auto range = outstandingReqs.equal_range(lineAddr);
    bool removed = false;
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == req) {
            outstandingReqs.erase(it);
            removed = true;
            DPRINTF(CXLCard, "Removed request %p from outstandingReqs for addr 0x%lx\n", req, lineAddr);
            break;
        }
    }
    if (!removed) {
         warn("Could not find request %p in outstandingReqs to remove for addr 0x%lx", req, lineAddr);
    }


    // Increment completed requests counter
    completedRequests++;

    // If all requests are completed, exit the simulation
    if (allRequestsCompleted()) {
        DPRINTF(CXLCard, "All %d requests completed. Exiting simulation.\n", totalRequests);
        exitSimLoop("All CXL requests completed", 0);
    }
}

void
CXLController::processCacheMiss(CXLRequest* req, PacketPtr missPkt)
{
    // Mark as cache miss (should already be done, but ensure)
    req->cacheHit = false;

    Addr blockAddr = req->addr & ~(cacheLineSize - 1);
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
    Addr lineAddr = req.addr & ~(cacheLineSize - 1);

    // Create the request
    auto memReq = std::make_shared<Request>(
        lineAddr, cacheLineSize, 0, 0);

    // Create the packet
    PacketPtr pkt = new Packet(memReq, req.isRead ?
                              MemCmd::ReadReq : MemCmd::WriteReq);

    // Set packet size and allocate memory if needed
    pkt->allocate();

    // If it's a write request, fill with some data
    if (!req.isRead) {
        std::memset(pkt->getPtr<uint8_t>(), 0xA5, cacheLineSize);
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
    Addr lineAddr = req.translatedAddr & ~(cacheLineSize - 1);
    DPRINTF(CXLCard, "Using translated address 0x%lx for memory request (Req %p)\n", lineAddr, &req);

    // Check if there's already a pending request for this address
    auto existingIt = outstandingMemReqs.find(lineAddr);
    if (existingIt != outstandingMemReqs.end()) {
        // Found an existing request to the same address
        CXLRequest* existingReq = existingIt->second;

        // Only merge read requests (not writes)
        if (req.isRead) {
            DPRINTF(CXLCard, "Found existing memory request for addr 0x%lx (Primary Req %p), merging current Req %p\n",
                   lineAddr, existingReq, &req);

            // Mark this request as waiting for the existing request
            req.isWaitingForMemory = true;
            req.waitingForRequest = existingReq;

            // Add this request to the dependent requests list
            dependentReqs[existingReq].push_back(&req);

            DPRINTF(CXLCard, "Added dependent request %p to primary request %p - now %u dependent requests\n",
                  &req, existingReq, dependentReqs[existingReq].size());

            // Don't need to send another request
            return true;
        }
         // else: Don't merge writes, proceed to send a new request
    }

    // Calculate the compressed size based on compression ratio
    // comprRatio is in percentage, e.g. 50.0 means 50% of original size
    unsigned compressedSize = std::max(static_cast<unsigned>(cacheLineSize * req.comprRatio / 100.0), 1u);

    DPRINTF(CXLCard, "Original size: %u bytes, Compressed size: %u bytes (%.2f%%)\n",
            cacheLineSize, compressedSize, req.comprRatio);

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
    Addr blockAddr = origAddr & ~(cacheLineSize - 1);

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
        DPRINTF(CXLCard, "Translation request failed for Req %p, adding transPkt %p to retry queue\n", &req, pkt);
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

    DPRINTF(CXLCard, "Processing request %p: Addr 0x%lx Time %.3f\n", trackedReq, trackedReq->addr, trackedReq->time_us);

    // Send address translation request in parallel with cache request
    sendAddressTranslationRequest(*trackedReq);

    // First try the cache
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
