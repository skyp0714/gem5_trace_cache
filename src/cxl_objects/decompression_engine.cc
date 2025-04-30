#include "cxl_objects/decompression_engine.hh"

#include "base/trace.hh"
#include "debug/DecompEngine.hh"
#include "sim/system.hh"

namespace gem5
{

DecompressionEngine::DecompressionEngine(const DecompressionEngineParams &params)
    : ClockedObject(params),
      cxlPort(name() + ".cxl_side_port", this),
      memPort(name() + ".mem_side_port", this),
      memoryStalled(false),
      responseStalled(false),
      respondingRequest(nullptr),
      block_size(params.block_size)
{
}

Port &
DecompressionEngine::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cxl_side_port") {
        return cxlPort;
    } else if (if_name == "mem_side_port") {
        return memPort;
    } else {
        return ClockedObject::getPort(if_name, idx);
    }
}

bool
DecompressionEngine::CXLSidePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(DecompEngine, "Received request for address %#x, size %d bytes\n",
            pkt->getAddr(), pkt->getSize());

    // If memory side port is stalled, we can't accept more requests
    if (owner->memoryStalled) {
        DPRINTF(DecompEngine, "Memory side stalled, can't accept request\n");
        return false;
    }

    // Create a new decompression request
    DecompressionRequest* req = new DecompressionRequest(pkt, curTick());

    // Extract compression ratio from the packet if available
    double comprRatio = 100.0; // Default to no compression
    if (pkt->hasData() && pkt->getSize() >= 8) {
        comprRatio = *reinterpret_cast<const double*>(pkt->getConstPtr<uint8_t>());
        DPRINTF(DecompEngine, "Extracted compression ratio: %.2f%%\n", comprRatio);
    }

    // Calculate full size after decompression
    unsigned fullSize = owner->block_size;
    unsigned compressedSize = pkt->getSize();

    DPRINTF(DecompEngine, "Compressed size: %u bytes, Full size after decompression: %u bytes\n",
            compressedSize, fullSize);

    // Forward the request to memory
    bool success = owner->memPort.sendTimingReq(pkt);

    if (success) {
        // Add to pending requests
        owner->pendingRequests[pkt->getAddr()] = req;
    } else {
        // Memory is stalled
        owner->memoryStalled = true;
        owner->requestQueue.push(req);
        DPRINTF(DecompEngine, "Memory stalled, queueing request\n");
    }

    return true;
}

void
DecompressionEngine::CXLSidePort::recvRespRetry()
{
    DPRINTF(DecompEngine, "Received response retry\n");

    if (owner->responseStalled && owner->respondingRequest) {
        // Try to send the response again
        PacketPtr pkt = owner->respondingRequest->pkt;
        bool success = sendTimingResp(pkt);

        if (success) {
            // Successfully sent the response
            DPRINTF(DecompEngine, "Sent previously stalled response to CXL controller\n");
            delete owner->respondingRequest;
            owner->respondingRequest = nullptr;
            owner->responseStalled = false;

            // Process next request if any
            owner->processNextRequest();
        }
    }
}

Tick
DecompressionEngine::CXLSidePort::recvAtomic(PacketPtr pkt)
{
    // Forward the request to memory
    return owner->memPort.sendAtomic(pkt);
}

void
DecompressionEngine::CXLSidePort::recvFunctional(PacketPtr pkt)
{
    // Check the pending requests and update the packet if it hits
    for (auto& pair : owner->pendingRequests) {
        if (pkt->trySatisfyFunctional(pair.second->pkt)) {
            return;
        }
    }

    // Also check the queue
    for (size_t i = 0; i < owner->requestQueue.size(); i++) {
        DecompressionRequest* req = owner->requestQueue.front();
        owner->requestQueue.pop();

        if (pkt->trySatisfyFunctional(req->pkt)) {
            owner->requestQueue.push(req);
            return;
        }

        owner->requestQueue.push(req);
    }

    // Forward to memory
    owner->memPort.sendFunctional(pkt);
}

AddrRangeList
DecompressionEngine::CXLSidePort::getAddrRanges() const
{
    // Simply return the same list as the memory side port
    return owner->memPort.getAddrRanges();
}

bool
DecompressionEngine::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(DecompEngine, "Received response from memory for address %#x\n", pkt->getAddr());

    // Pass to owner for handling
    owner->handleResponse(pkt);
    return true;
}

void
DecompressionEngine::MemSidePort::recvReqRetry()
{
    DPRINTF(DecompEngine, "Received request retry from memory\n");

    // We were stalled; try sending the next request
    owner->memoryStalled = false;
    owner->processNextRequest();
}

void
DecompressionEngine::handleResponse(PacketPtr pkt)
{
    // Look up the pending request for this response
    auto it = pendingRequests.find(pkt->getAddr());

    if (it == pendingRequests.end()) {
        panic("Received response for unknown address %#x\n", pkt->getAddr());
    }

    DecompressionRequest* req = it->second;

    if (pkt->isRead()) {
        DPRINTF(DecompEngine, "Starting decompression for address %#x\n", pkt->getAddr());

        // Schedule decompression to complete after the latency
        scheduleDecompression(req);
    } else {
        // For write requests, we don't need decompression, forward right away
        DPRINTF(DecompEngine, "Write response, forwarding immediately to CXL controller\n");

        // Try to send the response
        bool success = cxlPort.sendTimingResp(pkt);

        if (success) {
            // Remove from pending requests
            pendingRequests.erase(it);
            delete req;
        } else {
            // Stalled, mark as ready to respond and keep track of it
            responseStalled = true;
            req->readyToRespond = true;
            respondingRequest = req;
            DPRINTF(DecompEngine, "CXL port stalled, delaying response for %#x\n", pkt->getAddr());
        }
    }
}

void
DecompressionEngine::scheduleDecompression(DecompressionRequest* req)
{
    // Calculate decompression latency proportional to block size
    // Base latency is 200ns for 4KB blocks
    Tick decompTime = (block_size * 200000) / 4096; // Convert 200ns to ps (1ns = 1000ps)

    // Minimum latency of 10ns for very small blocks
    decompTime = std::max(decompTime, Tick(10000));

    // Schedule decompression event
    Tick completionTime = curTick() + decompTime;
    DecompressionEvent* event = new DecompressionEvent(this, req);
    schedule(event, completionTime);

    DPRINTF(DecompEngine, "Scheduled decompression to complete at tick %llu (latency: %llu ps, block size: %u bytes)\n",
           completionTime, decompTime, block_size);
}

void
DecompressionEngine::completeDecompression(DecompressionRequest* req)
{
    DPRINTF(DecompEngine, "Decompression complete for address %#x\n", req->pkt->getAddr());

    // Mark as ready to respond
    req->readyToRespond = true;

    // If not already stalled, try to send the response
    if (!responseStalled) {
        bool success = cxlPort.sendTimingResp(req->pkt);

        if (success) {
            // Remove from pending requests
            pendingRequests.erase(req->pkt->getAddr());
            delete req;
        } else {
            // Stalled
            responseStalled = true;
            respondingRequest = req;
            DPRINTF(DecompEngine, "CXL port stalled, delaying response for %#x\n", req->pkt->getAddr());
        }
    } else {
        // Already stalled, will be sent when unstalled
        // Keep it in pendingRequests
        DPRINTF(DecompEngine, "Already stalled, response for %#x will be sent later\n", req->pkt->getAddr());
    }
}

void
DecompressionEngine::processNextRequest()
{
    if (memoryStalled && !requestQueue.empty()) {
        DecompressionRequest* req = requestQueue.front();
        bool success = memPort.sendTimingReq(req->pkt);

        if (success) {
            // Request sent successfully
            requestQueue.pop();
            pendingRequests[req->pkt->getAddr()] = req;

            // If queue is empty, we're no longer stalled
            if (requestQueue.empty()) {
                memoryStalled = false;
            }
        }
    }
}

} // namespace gem5
