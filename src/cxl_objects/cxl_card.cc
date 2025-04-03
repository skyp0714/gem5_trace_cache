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
      stats(this),  // Initialize stats group
      totalRequests(0),
      completedRequests(0)
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

bool
CXLController::CXLRequestPort::recvTimingResp(PacketPtr pkt)
{
    // Safety check - ensure packet is valid
    if (!pkt) {
        warn("Received null packet in recvTimingResp!");
        return true;
    }

    if (isCache) {
        // Get the request address
        Addr addr = pkt->getAddr();

        // Check if this is a response to our WriteLineReq cache fill operation
        // In gem5, WriteLineReq gets a normal WriteResp reply
        if (pkt->isResponse() && pkt->cmd == MemCmd::WriteResp) {
            DPRINTF(CXLCard, "Received cache fill response for addr 0x%lx, ignoring as original request already completed\n", addr);
            // Since we've already completed the original request when we got the memory response,
            // we just need to clean up this fill response packet
            delete pkt;
            return true;
        }

        // Find the corresponding request in our tracking map
        auto it = controller->outstandingReqs.find(addr);
        if (it == controller->outstandingReqs.end()) {
            warn("Received response for unknown address: 0x%lx\n", addr);
            delete pkt;
            return true;
        }

        CXLRequest* req = it->second;

        // Use isError() instead of isFail() to detect cache miss responses
        // When a cache doesn't have a memory connection, it sets error flag on miss responses
        bool hit = pkt->isResponse() && !pkt->isError();

        DPRINTF(CXLCard, "Response from cache: %s, addr: 0x%lx, hit: %d, error: %d\n",
                pkt->cmdString(), addr, hit, pkt->isError());

        if (hit) {
            // Cache hit for either read or write
            DPRINTF(CXLCard, "Cache %s hit for address 0x%lx\n",
                   req->isRead ? "read" : "write", addr);

            // Mark request as a cache hit
            req->cacheHit = true;

            // Complete the request - either read or write hit
            controller->completeRequest(pkt);
        } else {
            // Cache miss for either read or write
            DPRINTF(CXLCard, "Cache %s miss for address 0x%lx\n",
                   req->isRead ? "read" : "write", addr);

            controller->processCacheMiss(pkt);
        }
    } else if (controller->translationPort.name() == name()) {
        // Handle address translation response
        Addr translationAddr = pkt->getAddr();

        DPRINTF(CXLCard, "Received address translation response for addr 0x%lx\n",
                translationAddr);

        // Find the original block address from the translation address
        Addr origBlockAddr = translationAddr - 0x40000000;

        // Need to search through all outstanding requests to find the matching one
        CXLRequest* req = nullptr;
        for (auto& pair : controller->outstandingReqs) {
            // Check if this request was waiting for translation and matches the block address
            Addr reqBlockAddr = pair.second->addr & ~(controller->cacheLineSize - 1);
            if (reqBlockAddr == origBlockAddr &&
                pair.second->translationSent && !pair.second->translationDone) {
                req = pair.second;
                DPRINTF(CXLCard, "Found matching request with addr 0x%lx (block addr 0x%lx)\n",
                       pair.second->addr, reqBlockAddr);
                break;
            }
        }

        // If request not found or already completed (cache hit), just ignore the response
        if (!req) {
            DPRINTF(CXLCard, "Translation response for completed/unknown request, ignoring\n");
            delete pkt;
            return true;
        }

        // Mark translation as complete
        req->translationDone = true;

        // Apply the translation - use the same address for now
        // In a real system, this would use actual translation data from the packet
        req->translatedAddr = req->addr;

        DPRINTF(CXLCard, "Address 0x%lx translated to 0x%lx\n", req->addr, req->translatedAddr);

        // If this was a cache miss waiting for translation, now send to memory
        if (!req->cacheHit && req->sentToCache) {
            controller->sendRequestToMemory(*req);
        }

        // Clean up the packet
        delete pkt;
    } else {
        // Response from memory (decompression engine)
        Addr addr = pkt->getAddr();

        DPRINTF(CXLCard, "Received response from decompression engine for addr 0x%lx\n", addr);

        // Find the request with matching translated address instead of original address
        CXLRequest* req = nullptr;
        for (auto& pair : controller->outstandingReqs) {
            if (pair.second->translationDone &&
                (pair.second->translatedAddr & ~(controller->cacheLineSize - 1)) == addr) {
                req = pair.second;
                DPRINTF(CXLCard, "Found matching request with original addr 0x%lx, translated addr 0x%lx\n",
                       pair.second->addr, pair.second->translatedAddr);
                break;
            }
        }

        if (!req) {
            warn("Received memory response for unknown translated address: 0x%lx\n", addr);
            delete pkt;
            return true;
        }

        // For read misses, fill the cache with data from decompression engine
        if (pkt->isRead() && req->isRead && !req->cacheHit) {
            // Create a cache fill request with decompressed data
            DPRINTF(CXLCard, "Creating cache fill request with data from decompression engine\n");

            // Use the original address for the cache fill (not the translated address)
            Addr cacheLineAddr = req->addr & ~(controller->cacheLineSize - 1);

            // Create the request for cache fill
            auto fillReq = std::make_shared<Request>(
                cacheLineAddr, controller->cacheLineSize, 0, 0);

            // Create a WriteLineReq packet to fill the cache
            PacketPtr fillPkt = new Packet(fillReq, MemCmd::WriteLineReq);
            fillPkt->allocate();
            fillPkt->setData(pkt->getConstPtr<uint8_t>());

            DPRINTF(CXLCard, "Sending cache fill request to addr 0x%lx with decompressed data\n",
                    cacheLineAddr);

            // Try to send the cache fill request
            bool success = controller->cachePort.sendTimingReq(fillPkt);
            if (!success) {
                DPRINTF(CXLCard, "Cache fill request failed, adding to retry queue\n");
                controller->cacheRetryQueue.push(fillPkt);
            } else {
                DPRINTF(CXLCard, "Successfully sent cache fill request. Cache fill response will be ignored.\n");
            }
        }

        // Complete the original request
        // Use the original address to find the request in outstandingReqs for completeRequest
        Addr origAddrAligned = req->addr & ~(controller->cacheLineSize - 1);
        // Temporarily change packet address to original address for completion
        Addr savedAddr = pkt->getAddr();
        pkt->setAddr(origAddrAligned);
        controller->completeRequest(pkt);
        // Restore original address in case the packet is used elsewhere
        pkt->setAddr(savedAddr);
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
CXLController::completeRequest(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();

    // Find the corresponding request
    auto it = outstandingReqs.find(addr);
    if (it == outstandingReqs.end()) {
        warn("Received response for unknown address: 0x%lx\n", addr);
        // Don't delete req - it's a shared_ptr that manages its own memory
        delete pkt;
        return;
    }

    CXLRequest* req = it->second;

    // Calculate request latency in ticks and convert to nanoseconds
    Tick latency_ticks = curTick() - req->sendTick;
    // Convert to nanoseconds (1 tick = 1 ps in gem5)
    double latency_ns = static_cast<double>(latency_ticks) / 1000.0;

    // We now know definitively if it was a cache hit
    bool isHit = req->cacheHit;
    bool isRead = req->isRead;

    // Record latency in the appropriate stats
    stats.meanAccessLatency = latency_ns;

    // Increment the appropriate counter statistics
    stats.totalRequests++;

    if (isRead) {
        stats.readLatency = latency_ns;
        if (isHit) {
            stats.hitLatency = latency_ns;
            stats.readHitLatency = latency_ns;
            stats.totalHits++;
            stats.readHits++;
        } else {
            stats.missLatency = latency_ns;
            stats.readMissLatency = latency_ns;
            stats.totalMisses++;
            stats.readMisses++;
        }
    } else {
        stats.writeLatency = latency_ns;
        if (isHit) {
            stats.hitLatency = latency_ns;
            stats.writeHitLatency = latency_ns;
            stats.totalHits++;
            stats.writeHits++;
        } else {
            stats.missLatency = latency_ns;
            stats.writeMissLatency = latency_ns;
            stats.totalMisses++;
            stats.writeMisses++;
        }
    }

    // Print completion information
    DPRINTF(CXLCard, "Completed CXL Request: %s Address: 0x%lx Time: %lu us Compression Ratio: %.2f Latency: %.2f ns (%s)\n",
           req->isRead ? "Read" : "Write",
           req->addr,
           req->time_us,
           req->comprRatio,
           latency_ns,
           isHit ? "cache hit" : "cache miss");

    // Clean up the packet
    // Don't delete req - it's a shared_ptr that manages its own memory
    delete pkt;

    // Remove from outstanding requests
    outstandingReqs.erase(it);

    // Increment completed requests counter
    completedRequests++;

    // If all requests are completed, exit the simulation
    if (allRequestsCompleted()) {
        DPRINTF(CXLCard, "All %d requests completed. Exiting simulation.\n", totalRequests);
        exitSimLoop("All CXL requests completed", 0);
    }
}

void
CXLController::processCacheMiss(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();

    // Find the corresponding request
    auto it = outstandingReqs.find(addr);
    if (it == outstandingReqs.end()) {
        warn("Received cache miss for unknown address: 0x%lx\n", addr);
        delete pkt;
        return;
    }

    CXLRequest* req = it->second;

    // Mark as cache miss
    req->cacheHit = false;

    // Print both the block address and the original request address for clarity
    DPRINTF(CXLCard, "Cache miss for block address 0x%lx (request addr 0x%lx), checking translation\n",
            addr, req->addr);

    // Clean up the cache packet
    delete pkt;

    // Only send read misses to memory (write misses are ignored with no-write-allocate)
    if (req->isRead) {
        // Check if translation is already done
        if (req->translationDone) {
            // Translation already complete, proceed with memory access
            DPRINTF(CXLCard, "Translation already complete, proceeding with memory request\n");
            sendRequestToMemory(*req);
        } else {
            // Translation not done yet, will send to memory when translation completes
            DPRINTF(CXLCard, "Waiting for address translation to complete\n");
            // The translation response handler will call sendRequestToMemory when translation completes
        }
    } else {
        // For write misses with no-write-allocate, we should have already completed the request
        // This code path should not be reached for write misses
        warn("Unexpected write miss processing in processCacheMiss for addr 0x%lx\n", addr);
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

    // Add to outstanding requests map
    int index = &req - &requests[0];
    if (index >= 0 && index < static_cast<int>(requests.size())) {
        CXLRequest* trackedReq = &requests[index];
        outstandingReqs[lineAddr] = trackedReq;
    } else {
        // Create a temporary copy in the requests vector
        requests.push_back(req);
        outstandingReqs[lineAddr] = &requests.back();
    }

    // Print request information
    DPRINTF(CXLCard, "Sending CXL Request to Cache: %s Address: 0x%lx Time: %lu us Compression Ratio: %.2f\n",
           req.isRead ? "Read" : "Write",
           req.addr,
           req.time_us,
           req.comprRatio);

    // Send the packet to cache
    bool success = cachePort.sendTimingReq(pkt);
    return success;
}

bool
CXLController::sendRequestToMemory(CXLRequest &req)
{
    // Always use the translated address
    assert(req.translationDone && "Translation must be complete before sending to memory");

    Addr lineAddr = req.translatedAddr & ~(cacheLineSize - 1);
    DPRINTF(CXLCard, "Using translated address 0x%lx for memory request\n", lineAddr);

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
    if (pkt->hasData()) {
        *reinterpret_cast<double*>(pkt->getPtr<uint8_t>()) = req.comprRatio;
    }

    // Store the memory packet in the request
    req.memPkt = pkt;

    // Print request information with addresses and compression ratio
    DPRINTF(CXLCard, "Sending CXL Request to Memory: %s Address: 0x%lx (orig: 0x%lx) Size: %u bytes, Ratio: %.2f%%\n",
           req.isRead ? "Read" : "Write",
           lineAddr, req.addr, compressedSize, req.comprRatio);

    // Send the packet directly to memory
    bool success = memPort.sendTimingReq(pkt);
    return success;
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

    DPRINTF(CXLCard, "Translation lookup: original addr 0x%lx (block addr 0x%lx) → table lookup addr 0x%lx\n",
            origAddr, blockAddr, translationAddr);

    // Create request and packet - explicitly use 8 bytes (64 bits) for translation lookup
    auto transReq = std::make_shared<Request>(translationAddr, 8, 0, 0);
    PacketPtr pkt = new Packet(transReq, MemCmd::ReadReq);
    pkt->allocate();

    // Store in request
    req.transPkt = pkt;
    req.translationSent = true;

    // Send request
    bool success = translationPort.sendTimingReq(pkt);
    if (!success) {
        DPRINTF(CXLCard, "Translation request failed, adding to retry queue\n");
        translationRetryQueue.push(pkt);
    }

    return success;
}

void
CXLController::processRequest(const CXLRequest &reqEvent)
{
    // Find the request in our vector to get a properly tracked reference
    // that won't go out of scope when this function returns
    CXLRequest* trackedReq = nullptr;
    for (size_t i = 0; i < requests.size(); i++) {
        if (requests[i].addr == reqEvent.addr &&
            requests[i].time_us == reqEvent.time_us) {
            trackedReq = &requests[i];
            break;
        }
    }

    if (trackedReq == nullptr) {
        warn("Could not find matching request in request vector, skipping");
        return;
    }

    // Send address translation request in parallel with cache request
    sendAddressTranslationRequest(*trackedReq);

    // First try the cache
    if (!sendRequestToCache(*trackedReq)) {
        // If sending fails, add to retry queue
        if (trackedReq->pkt) {
            DPRINTF(CXLCard, "Cache request enqueued for retry: %s Address: 0x%lx\n",
                   trackedReq->isRead ? "Read" : "Write", trackedReq->addr);
            cacheRetryQueue.push(trackedReq->pkt);
        } else {
            warn("Failed to send request but packet was not created!");
        }
    }
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
        uint64_t time_us;
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
    DPRINTF(CXLCard, "Loaded %zu CXL requests from trace file: %s\n",
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

} // namespace gem5
