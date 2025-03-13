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
    }
}

Port &
CXLController::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cache_port") {
        return cachePort;
    } else if (if_name == "mem_port") {
        return memPort;
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
        // Check if this packet is from cache and has error flag (indicating a miss)
        bool hit = pkt->cacheResponding() && !pkt->isError();
        
        if (hit) {
            // On cache hit, mark the request as a hit and complete it
            Addr addr = pkt->getAddr();
            auto it = controller->outstandingReqs.find(addr);
            if (it != controller->outstandingReqs.end()) {
                it->second->cacheHit = true;
            }
            controller->completeRequest(pkt);
        } else {
            // On cache miss or error, forward to memory
            controller->processCacheMiss(pkt);
        }
    } else {
        // Response from memory, always complete the request
        controller->completeRequest(pkt);
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
    
    // Calculate request latency
    Tick latency = curTick() - req->sendTick;
    
    // We now know definitively if it was a cache hit
    bool isHit = req->cacheHit;
    
    // Print completion information
    DPRINTF(CXLCard, "Completed CXL Request: %s Address: 0x%lx Time: %lu us Compression Ratio: %.2f Latency: %lu ticks (%s)\n", 
           req->isRead ? "Read" : "Write",
           req->addr,
           req->time_us,
           req->comprRatio,
           latency, 
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
    
    DPRINTF(CXLCard, "Cache miss for address 0x%lx, sending to memory\n", addr);
    
    // Clean up the cache packet
    delete pkt;
    
    // Send directly to memory
    if (!sendRequestToMemory(*req)) {
        // If sending to memory fails, add to retry queue
        if (req->memPkt) {
            DPRINTF(CXLCard, "Memory request enqueued for retry: %s Address: 0x%lx\n", 
                   req->isRead ? "Read" : "Write", req->addr);
            memRetryQueue.push(req->memPkt);
        } else {
            warn("Failed to create memory request for cache miss!");
        }
    }
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
    // Calculate the cache line address
    Addr lineAddr = req.addr & ~(cacheLineSize - 1);
    
    // Create the request for memory
    auto memReq = std::make_shared<Request>(
        lineAddr, cacheLineSize, 0, 0);
    
    // Create the packet for direct memory access
    PacketPtr pkt = new Packet(memReq, req.isRead ? 
                              MemCmd::ReadReq : MemCmd::WriteReq);
    
    // Set packet size and allocate memory if needed
    pkt->allocate();
    
    // If it's a write request, fill with some data
    if (!req.isRead) {
        std::memset(pkt->getPtr<uint8_t>(), 0xA5, cacheLineSize);
    }
    
    // Store the memory packet in the request
    req.memPkt = pkt;
    
    // Print request information
    DPRINTF(CXLCard, "Sending CXL Request directly to Memory: %s Address: 0x%lx\n", 
           req.isRead ? "Read" : "Write",
           req.addr);
    
    // Send the packet directly to memory
    bool success = memPort.sendTimingReq(pkt);
    return success;
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

