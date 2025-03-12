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
      memPort(name() + ".mem_side_port", this),
      totalRequests(0),
      completedRequests(0)
{
}

CXLController::~CXLController()
{
    // Clean up any packets in the retry queue
    while (!retryQueue.empty()) {
        PacketPtr pkt = retryQueue.front();
        retryQueue.pop();
        // Don't delete req - it's a shared_ptr that manages its own memory
        delete pkt;
    }
    
    // Clean up any outstanding request packets
    for (auto& pair : outstandingReqs) {
        if (pair.second->pkt) {
            // Don't delete req - it's a shared_ptr that manages its own memory
            delete pair.second->pkt;
            pair.second->pkt = nullptr;
        }
    }
}

Port &
CXLController::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side_port") {
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
    
    // Handle the response from memory
    controller->completeRequest(pkt);
    return true;
}

void
CXLController::CXLRequestPort::recvReqRetry()
{
    // Retry sending any packets in the retry queue
    controller->trySendRetries();
}

void 
CXLController::trySendRetries()
{
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
        DPRINTF(CXLCard, "Attempting to retry packet for addr 0x%lx\n", pkt->getAddr());
        
        if (!memPort.sendTimingReq(pkt)) {
            // Still blocked, will retry later
            DPRINTF(CXLCard, "Retry sending packet for addr 0x%lx still blocked\n", 
                   pkt->getAddr());
            return;
        }
        
        DPRINTF(CXLCard, "Successfully resent packet for addr 0x%lx\n", 
               pkt->getAddr());
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
    
    // Print completion information
    DPRINTF(CXLCard, "Completed CXL Request: %s Address: 0x%lx Time: %lu us Compression Ratio: %.2f\n", 
           req->isRead ? "Read" : "Write",
           req->addr,
           req->time_us,
           req->comprRatio);
    
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
    
    // Create and send the request to the cache using the persistent request
    if (!sendRequest(*trackedReq)) {
        // Ensure the packet was created before adding to retry queue
        if (trackedReq->pkt) {
            DPRINTF(CXLCard, "Request enqueued for retry: %s Address: 0x%lx\n", 
                   trackedReq->isRead ? "Read" : "Write", trackedReq->addr);
            retryQueue.push(trackedReq->pkt);
        } else {
            warn("Failed to send request but packet was not created!");
        }
    }
}

bool
CXLController::sendRequest(CXLRequest &req)
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
    DPRINTF(CXLCard, "Sending CXL Request: %s Address: 0x%lx Time: %lu us Compression Ratio: %.2f\n", 
           req.isRead ? "Read" : "Write",
           req.addr,
           req.time_us,
           req.comprRatio);
    
    // Send the packet
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

