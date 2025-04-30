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
      block_size(params.block_size),
      num_engines(params.num_engines), // Ensure num_engines is initialized
      active_decompressions(0)
{
    fatal_if(num_engines == 0, "DecompressionEngine must have at least one engine.");
    DPRINTF(DecompEngine, "Initialized with %u decompression engines.\n", num_engines);
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

    // Create a new decompression request object immediately
    DecompressionRequest* req = new DecompressionRequest(pkt, curTick());

    // If memory side port is stalled from previous attempts, queue for retry
    if (owner->memoryStalled) {
        DPRINTF(DecompEngine, "Memory side stalled, queueing request %p for retry\n", req);
        owner->memoryRetryQueue.push(req); // Use memoryRetryQueue
        return true; // Accept the request into the retry queue
    }

    // Extract compression ratio (remains the same)
    double comprRatio = 100.0; // Default to no compression
    if (pkt->hasData() && pkt->getSize() >= 8) {
        comprRatio = *reinterpret_cast<const double*>(pkt->getConstPtr<uint8_t>());
        DPRINTF(DecompEngine, "Extracted compression ratio: %.2f%%\n", comprRatio);
    }

    // Try Forwarding the request to memory immediately
    bool success = owner->memPort.sendTimingReq(pkt);

    if (success) {
        // Add to pending requests map (waiting for memory response)
        owner->pendingRequests[pkt->getAddr()] = req;
        DPRINTF(DecompEngine, "Request %p sent to memory, added to pendingRequests\n", req);
    } else {
        // Memory port busy, queue for retry and set stalled flag
        owner->memoryStalled = true;
        owner->memoryRetryQueue.push(req); // Use memoryRetryQueue
        DPRINTF(DecompEngine, "Memory port busy, queueing request %p for retry\n", req);
    }

    return true; // Always accept the request (either sent or queued for retry)
}

void
DecompressionEngine::CXLSidePort::recvRespRetry()
{
    DPRINTF(DecompEngine, "Received response retry from CXL controller\n");

    // First, try sending the primary stalled response
    if (owner->responseStalled && owner->respondingRequest) {
        PacketPtr pkt_to_send = owner->respondingRequest->pkt;
        bool success = sendTimingResp(pkt_to_send);

        if (success) {
            DPRINTF(DecompEngine, "Sent previously stalled response for req %p to CXL controller\n", owner->respondingRequest);
            delete owner->respondingRequest; // Clean up the request object
            owner->respondingRequest = nullptr;
            owner->responseStalled = false; // Stall cleared

            // Now, try sending any other completed requests that were waiting
            while (!owner->responseStalled && !owner->completed_queue.empty()) {
                 DecompressionRequest* next_req = owner->completed_queue.front();
                 DPRINTF(DecompEngine, "Attempting to send next completed req %p from completed_queue.\n", next_req);
                 bool next_success = sendTimingResp(next_req->pkt);
                 if (next_success) {
                     owner->completed_queue.pop();
                     delete next_req; // Clean up
                 } else {
                     // Port stalled again
                     owner->responseStalled = true;
                     owner->respondingRequest = next_req; // This is now the primary stalled request
                     owner->completed_queue.pop(); // Remove from completed queue
                     DPRINTF(DecompEngine, "CXL port stalled again while sending from completed_queue (req %p).\n", next_req);
                     break; // Stop processing completed queue
                 }
            }

            // Try scheduling next decompression if stall cleared and engines might be free
            if (!owner->responseStalled) {
                owner->tryScheduleDecompression(); // Use tryScheduleDecompression
            }

        } else {
             DPRINTF(DecompEngine, "Retry sending response for req %p failed, still stalled\n", owner->respondingRequest);
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

    // Check the memory retry queue
    // Need to iterate carefully as queue doesn't support direct iteration
    std::queue<DecompressionRequest*> temp_queue;
    bool found = false;
    while (!owner->memoryRetryQueue.empty()) { // Use memoryRetryQueue
        DecompressionRequest* req = owner->memoryRetryQueue.front();
        owner->memoryRetryQueue.pop();
        if (!found && pkt->trySatisfyFunctional(req->pkt)) {
            found = true;
            // Keep req in the queue if found
        }
        temp_queue.push(req);
    }
    owner->memoryRetryQueue = std::move(temp_queue); // Restore queue
    if (found) return;


    // Check the decompression queue
    while (!owner->decompression_queue.empty()) { // Use decompression_queue
        DecompressionRequest* req = owner->decompression_queue.front();
        owner->decompression_queue.pop();
        if (!found && pkt->trySatisfyFunctional(req->pkt)) {
            found = true;
        }
        temp_queue.push(req);
    }
    owner->decompression_queue = std::move(temp_queue); // Restore queue
    if (found) return;

    // Check the completed queue
    while (!owner->completed_queue.empty()) { // Use completed_queue
        DecompressionRequest* req = owner->completed_queue.front();
        owner->completed_queue.pop();
        if (!found && pkt->trySatisfyFunctional(req->pkt)) {
            found = true;
        }
        temp_queue.push(req);
    }
    owner->completed_queue = std::move(temp_queue); // Restore queue
    if (found) return;


    // Forward to memory if not found anywhere
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

    // We were stalled; try sending the next request from the memory retry queue
    owner->memoryStalled = false;
    owner->trySendMemoryRetries(); // Use trySendMemoryRetries
}

void
DecompressionEngine::handleResponse(PacketPtr pkt)
{
    // Look up the pending request for this response using the address
    auto it = pendingRequests.find(pkt->getAddr());

    if (it == pendingRequests.end()) {
        // This might happen if the request was already handled (e.g., write response completed)
        // Or if it's a response we didn't expect.
        warn("Received memory response for address %#x, but no matching pending request found. Ignoring.\n", pkt->getAddr());
        delete pkt; // Clean up the unexpected packet
        return;
    }

    DecompressionRequest* req = it->second;
    // Remove from pendingRequests map as it's no longer waiting for memory response
    pendingRequests.erase(it);

    if (pkt->isRead()) {
        DPRINTF(DecompEngine, "Read response received for req %p (addr %#x). Queueing for decompression.\n", req, pkt->getAddr());
        // Add to the queue of requests waiting for a decompression engine
        decompression_queue.push(req);
        // Try to schedule it if an engine is free
        tryScheduleDecompression();
    } else { // Write response
        // For write requests, we don't need decompression, forward response to CXL immediately
        DPRINTF(DecompEngine, "Write response received for req %p (addr %#x). Forwarding immediately.\n", req, pkt->getAddr());

        // Try to send the response back to the CXL controller
        bool success = cxlPort.sendTimingResp(pkt); // Use the received packet directly

        if (success) {
            // Successfully sent, clean up the request object
            DPRINTF(DecompEngine, "Successfully forwarded write response for req %p.\n", req);
            delete req; // Delete the DecompressionRequest wrapper
        } else {
            // CXL port stalled, mark response as stalled
            responseStalled = true;
            req->readyToRespond = true; // Mark the request as ready
            respondingRequest = req;    // Store the request object
            DPRINTF(DecompEngine, "CXL port stalled, delaying write response for req %p (addr %#x)\n", req, pkt->getAddr());
        }
    }
}

void
DecompressionEngine::tryScheduleDecompression()
{
    // While there are free engines and requests waiting in the queue
    while (active_decompressions < num_engines && !decompression_queue.empty()) {
        // Get the next request from the queue
        DecompressionRequest* req_to_schedule = decompression_queue.front();
        decompression_queue.pop();

        // Increment active count
        active_decompressions++;

        DPRINTF(DecompEngine, "Starting decompression for req %p (addr %#x). Active engines: %u/%u\n",
               req_to_schedule, req_to_schedule->pkt->getAddr(), active_decompressions, num_engines);

        // Schedule the actual decompression event
        scheduleDecompression(req_to_schedule);
    }
     DPRINTF(DecompEngine, "tryScheduleDecompression finished. Active engines: %u/%u. Queue size: %u\n",
            active_decompressions, num_engines, decompression_queue.size());
}

void
DecompressionEngine::scheduleDecompression(DecompressionRequest* req)
{
    // Calculate decompression latency (same logic as before)
    Tick decompTime = (block_size * 150000) / 4096;
    decompTime = std::max(decompTime, Tick(10000));

    // Schedule decompression completion event
    Tick completionTime = clockEdge() + decompTime; // Use clockEdge() for proper scheduling
    DecompressionEvent* event = new DecompressionEvent(this, req);
    schedule(event, completionTime);

    DPRINTF(DecompEngine, "Scheduled decompression for req %p (addr %#x) to complete at tick %llu (latency: %llu ps)\n",
           req, req->pkt->getAddr(), completionTime, decompTime);
}

void
DecompressionEngine::completeDecompression(DecompressionRequest* req)
{
    DPRINTF(DecompEngine, "Decompression complete for req %p (addr %#x).\n", req, req->pkt->getAddr());

    // Decrement active count as this engine is now free
    assert(active_decompressions > 0);
    active_decompressions--;

    // Mark as ready to respond (used if CXL port is stalled)
    req->readyToRespond = true;

    // If CXL port is not already stalled waiting for another response, try to send this one
    if (!responseStalled) {
        bool success = cxlPort.sendTimingResp(req->pkt);

        if (success) {
            // Successfully sent, clean up the request object
            DPRINTF(DecompEngine, "Successfully sent decompressed response for req %p.\n", req);
            delete req;
        } else {
            // CXL port stalled, store this request as the one waiting
            responseStalled = true;
            respondingRequest = req;
            DPRINTF(DecompEngine, "CXL port stalled, delaying decompressed response for req %p (addr %#x)\n", req, req->pkt->getAddr());
        }
    } else {
        // Already stalled, add to the completed queue to be sent later
        // DPRINTF(DecompEngine, "Already stalled, response for %#x will be sent later\n", req->pkt->getAddr()); // Original message
        // --- Adding `completed_queue` ---
        completed_queue.push(req);
        DPRINTF(DecompEngine, "CXL port stalled, adding completed req %p to completed_queue.\n", req);
        // --- End Adding `completed_queue` ---
    }

    // Try to schedule another decompression task now that this one finished
    tryScheduleDecompression();
}

// Renamed from processNextRequest
void DecompressionEngine::trySendMemoryRetries()
{
    // While the memory port is not stalled (or we assume it's not) and the retry queue is not empty
    while (!memoryStalled && !memoryRetryQueue.empty()) {
        DecompressionRequest* req_to_retry = memoryRetryQueue.front();
        DPRINTF(DecompEngine, "Attempting to retry sending req %p (addr %#x) to memory.\n",
               req_to_retry, req_to_retry->pkt->getAddr());

        bool success = memPort.sendTimingReq(req_to_retry->pkt);

        if (success) {
            // Request sent successfully
            memoryRetryQueue.pop(); // Remove from retry queue
            pendingRequests[req_to_retry->pkt->getAddr()] = req_to_retry; // Add to map waiting for response
            DPRINTF(DecompEngine, "Successfully resent req %p to memory.\n", req_to_retry);
        } else {
            // Memory port still busy
            memoryStalled = true;
            DPRINTF(DecompEngine, "Memory port still busy after retry for req %p.\n", req_to_retry);
            return; // Stop trying for now
        }
    }
}

} // namespace gem5
