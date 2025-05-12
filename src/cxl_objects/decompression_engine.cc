#include "cxl_objects/decompression_engine.hh"

#include "base/trace.hh"
#include "debug/DecompEngine.hh"
#include "sim/system.hh"
#include "mem/packet_access.hh" // For packet data access

namespace gem5
{

// ADDED: Definition for static const member
const Tick DecompressionEngine::interMemoryRequestDelay;

// ChunkSendEvent process method
void
DecompressionEngine::ChunkSendEvent::process() {
    engine->sendNextChunk(parentRequest, nextChunkIndex);
}

// ScheduleMemSendEvent process method
void
DecompressionEngine::ScheduleMemSendEvent::process() {
    engine->memSendEventScheduled = false; // Event is now processing

    if (engine->memoryStalled) {
        DPRINTF(DecompEngine, "MemSendEvent: Memory port stalled, deferring send.\n");
        // Do not reschedule here; recvReqRetry will trigger tryScheduleNextMemSend
        return;
    }

    if (engine->memSendCandidateQueue.empty()) {
        DPRINTF(DecompEngine, "MemSendEvent: Queue is empty, nothing to send.\n");
        return;
    }

    DecompressionRequest* req_to_send = engine->memSendCandidateQueue.front();
    // Packet must exist
    assert(req_to_send && req_to_send->pkt);

    DPRINTF(DecompEngine, "MemSendEvent: Attempting to send req %p (pkt addr %#x, type: %s) to memory.\n",
           req_to_send, req_to_send->pkt->getAddr(), req_to_send->isChunk ? "Chunk" : "Non-Chunked");

    bool success = engine->memPort.sendTimingReq(req_to_send->pkt);

    if (success) {
        engine->memSendCandidateQueue.pop();
        engine->pendingRequests[req_to_send->pkt->getAddr()] = req_to_send;
        engine->nextMemSendAvailableAt = engine->clockEdge() + interMemoryRequestDelay;
        DPRINTF(DecompEngine, "MemSendEvent: Successfully sent req %p to memory. Next send available at %llu.\n",
                req_to_send, engine->nextMemSendAvailableAt);
    } else {
        engine->memoryStalled = true;
        DPRINTF(DecompEngine, "MemSendEvent: Memory port busy for req %p. Will retry upon recvReqRetry.\n", req_to_send);
        // Request remains at the front of memSendCandidateQueue
    }

    // Try to schedule the next send if conditions allow
    engine->tryScheduleNextMemSend();
}

DecompressionEngine::DecompressionEngine(const DecompressionEngineParams &params)
    : ClockedObject(params),
      cxlPort(name() + ".cxl_side_port", this),
      memPort(name() + ".mem_side_port", this),
      memoryStalled(false),
      responseStalled(false),
      respondingRequest(nullptr),
      block_size(params.block_size),
      cache_line_size(params.cache_line_size),
      num_engines(params.num_engines), // Ensure num_engines is initialized
      active_decompressions(0),
      chunkSendDelay(1), // Default to 1 tick if params.chunk_send_delay_ticks is not available. Add to DecompressionEngineParams if needed.
      cxlReqStalled(false), // Initialize cxlReqStalled
      nextMemSendAvailableAt(0), // Initialize
      memSendEvent(this),        // Initialize ScheduleMemSendEvent
      memSendEventScheduled(false) // Initialize
{
    fatal_if(num_engines == 0, "DecompressionEngine must have at least one engine.");
    DPRINTF(DecompEngine, "Initialized with %u decompression engines. Chunk creation delay: %llu ticks. Mem send delay: %llu ticks.\n",
            num_engines, chunkSendDelay, interMemoryRequestDelay);
}

DecompressionEngine::~DecompressionEngine()
{
    DPRINTF(DecompEngine, "DEBUG: DecompressionEngine destructor called\n");

    // Log counts before cleanup
    DPRINTF(DecompEngine, "DEBUG: pendingRequests: %u, memSendCandidateQueue: %u, decompression_queue: %u, completed_queue: %u\n",
            pendingRequests.size(), memSendCandidateQueue.size(), decompression_queue.size(), completed_queue.size());

    // Clear pendingRequests (these are typically chunks)
    for (auto& pair : pendingRequests) {
        DecompressionRequest* req = pair.second;
        DPRINTF(DecompEngine, "DEBUG: Cleaning up pendingRequest %p (addr %#x)\n", req, pair.first);

        if (req) {
            // If it's a chunk, its pkt is owned by DecompressionEngine
            if (req->isChunk) {
                if (req->pkt) {
                    DPRINTF(DecompEngine, "DEBUG: Deleting chunk pkt %p\n", req->pkt);
                    delete req->pkt;
                    req->pkt = nullptr;
                }
            }
            // respPkt is the response from memory for this chunk
            if (req->respPkt) {
                DPRINTF(DecompEngine, "DEBUG: Deleting respPkt %p\n", req->respPkt);
                delete req->respPkt;
                req->respPkt = nullptr;
            }

            DPRINTF(DecompEngine, "DEBUG: Deleting request %p\n", req);
            delete req; // Delete the DecompressionRequest object for the chunk
        }
    }
    pendingRequests.clear();

    // Clear memSendCandidateQueue
    while (!memSendCandidateQueue.empty()) {
        DecompressionRequest* req = memSendCandidateQueue.front();
        memSendCandidateQueue.pop();
        if (req) {
            if (req->isChunk) { // If it's a chunk, its pkt is owned by DecompressionEngine
                delete req->pkt;
            }
            // If parent, pkt is from CXLController.
            // respPkt is usually null here or managed by DecompressionRequest destructor.
            delete req;
        }
    }

    // Clear decompression_queue (these are typically parent requests ready for decompression)
    while (!decompression_queue.empty()) {
        DecompressionRequest* req = decompression_queue.front();
        decompression_queue.pop();
        // pkt is from CXLController. respPkt might be an aggregated packet (if used).
        // DecompressionRequest destructor handles responseData.
        // If respPkt was an aggregated packet created by DecompEngine, it should be deleted.
        // Current design reuses req->pkt for final response, so req->respPkt might be null or temporary.
        delete req->respPkt; // If it was used for aggregation and not the final pkt
        delete req;
    }

    // Clear completed_queue (parent requests waiting to send to CXL)
    while (!completed_queue.empty()) {
        DecompressionRequest* req = completed_queue.front();
        completed_queue.pop();
        // Similar to decompression_queue
        delete req->respPkt;
        delete req;
    }

    // Clear memParentRetryQueue
     while (!memParentRetryQueue.empty()) {
        DecompressionRequest* req = memParentRetryQueue.front();
        memParentRetryQueue.pop();
        // This is a parent request. Its pkt is from CXLController.
        // Its DecompressionRequest destructor will handle its chunks and responseData.
        delete req;
    }


    // Clear cxlRequestRetryQueue (these are PacketPtr directly from CXLController)
    while (!cxlRequestRetryQueue.empty()) {
        PacketPtr pkt = cxlRequestRetryQueue.front();
        cxlRequestRetryQueue.pop();
        delete pkt; // These are original request packets that were stalled
    }

    // respondingRequest might still hold a request
    if (respondingRequest) {
        // Logic depends on what respondingRequest holds (parent/chunk)
        // Assuming it's a parent request whose final response is stalled.
        delete respondingRequest->respPkt; // If any
        delete respondingRequest;
        respondingRequest = nullptr;
    }
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
    DPRINTF(DecompEngine, "Received request for addr %#x, size %u from CXL controller.\n",
            pkt->getAddr(), pkt->getSize());

    // Check if request size is a multiple of cache_line_size
    if (pkt->getSize() % owner->cache_line_size != 0) {
        warn("Request size %u is not a multiple of cache_line_size %u. This may cause issues.",
             pkt->getSize(), owner->cache_line_size);
    }

    if (owner->cxlReqStalled) {
        DPRINTF(DecompEngine, "CXL side port stalled, queueing request for addr %#x.\n", pkt->getAddr());
        owner->cxlRequestRetryQueue.push(pkt);
        return true; // Accepted into retry queue
    }

    DecompressionRequest* parentReq = new DecompressionRequest(pkt, curTick());

    // For read requests larger than cache_line_size, split into chunks
    if (pkt->isRead() && pkt->getSize() > owner->cache_line_size) {
        // Calculate the exact number of chunks - ensure all chunks have valid data
        parentReq->totalChunks = pkt->getSize() / owner->cache_line_size;
        if (pkt->getSize() % owner->cache_line_size != 0) {
            parentReq->totalChunks++; // Add chunk only if there is a remainder
        }

        parentReq->responseData = new char[pkt->getSize()]; // Buffer for aggregated data
        // Initialize responseData to zeros or some known pattern if necessary
        std::memset(parentReq->responseData, 0, pkt->getSize());

        DPRINTF(DecompEngine, "Splitting read request for addr %#x (size %u) into %u chunks of %u bytes each (last chunk may be smaller).\n",
                pkt->getAddr(), pkt->getSize(), parentReq->totalChunks, owner->cache_line_size);

        // Send the first chunk immediately
        owner->sendNextChunk(parentReq, 0);
    } else { // Write requests or small read requests are not chunked
        DPRINTF(DecompEngine, "Processing non-chunked request for addr %#x (size %u).\n",
                pkt->getAddr(), pkt->getSize());
        // Add to memSendCandidateQueue and try to schedule
        owner->memSendCandidateQueue.push(parentReq);
        DPRINTF(DecompEngine, "Added non-chunked request %p (addr %#x) to memSendCandidateQueue.\n",
                parentReq, parentReq->pkt->getAddr());
        owner->tryScheduleNextMemSend();
    }
    return true; // Always accept the request
}

void
DecompressionEngine::sendNextChunk(DecompressionRequest* parentReq, unsigned chunkIndex)
{
    if (chunkIndex >= parentReq->totalChunks) {
        // Should not happen if logic is correct
        warn("sendNextChunk called with invalid chunkIndex %u for parentReq %p (total: %u)\n",
             chunkIndex, parentReq, parentReq->totalChunks);
        return;
    }

    Addr baseAddr = parentReq->pkt->getAddr();
    unsigned fullSize = parentReq->pkt->getSize();
    unsigned offset = chunkIndex * cache_line_size;

    // Calculate remaining size for the last chunk, otherwise use cache_line_size
    unsigned chunkSize;
    if (offset + cache_line_size > fullSize) {
        chunkSize = fullSize - offset; // Request only the remaining data size
    } else {
        chunkSize = cache_line_size;
    }

    // Check if chunk size is 0 (prevent bugs)
    if (chunkSize == 0) {
        warn("Calculated chunk size is 0 for chunkIndex %u, offset %u, fullSize %u. Skipping chunk.",
             chunkIndex, offset, fullSize);
        return;
    }

    Addr chunkAddr = baseAddr + offset;

    DPRINTF(DecompEngine, "Creating chunk %u/%u for addr %#x: offset=%u, chunkSize=%u, chunkAddr=%#x, fullSize=%u\n",
            chunkIndex + 1, parentReq->totalChunks, baseAddr, offset, chunkSize, chunkAddr, fullSize);

    // Create a new packet for the chunk
    // The request for the chunk should be a ReadReq
    auto chunk_internal_req = std::make_shared<Request>(chunkAddr, chunkSize, 0, parentReq->pkt->req->requestorId());
    PacketPtr chunkPkt = new Packet(chunk_internal_req, MemCmd::ReadReq);
    chunkPkt->allocate(); // Allocate data for the chunk packet (memory will fill it)

    DecompressionRequest* chunkReqObj = new DecompressionRequest(chunkPkt, curTick());
    chunkReqObj->isChunk = true;
    chunkReqObj->parentRequest = parentReq;
    chunkReqObj->chunkIndex = chunkIndex;
    // totalChunks and completedChunks are relevant for parentReq

    parentReq->chunkRequests.push_back(chunkReqObj); // Track the chunk

    DPRINTF(DecompEngine, "ParentReq %p: Sending chunk %u/%u for addr %#x (chunk addr %#x, size %u).\n",
            parentReq, chunkIndex + 1, parentReq->totalChunks, baseAddr, chunkAddr, chunkSize);

    // Add chunk to memSendCandidateQueue and try to schedule
    memSendCandidateQueue.push(chunkReqObj);
    DPRINTF(DecompEngine, "Added chunk %u (req %p, addr %#x) to memSendCandidateQueue.\n",
            chunkIndex, chunkReqObj, chunkAddr);
    tryScheduleNextMemSend();


    // Schedule next chunk if any
    unsigned nextChunkIndex_to_schedule = chunkIndex + 1;
    if (nextChunkIndex_to_schedule < parentReq->totalChunks) {
        DPRINTF(DecompEngine, "ParentReq %p: Scheduling next chunk %u with delay %llu ticks.\n",
                parentReq, nextChunkIndex_to_schedule, chunkSendDelay);
        ChunkSendEvent* event = new ChunkSendEvent(this, parentReq, nextChunkIndex_to_schedule);
        schedule(event, curTick() + chunkSendDelay);
    }
}

void
DecompressionEngine::CXLSidePort::recvRespRetry()
{
    DPRINTF(DecompEngine, "Received response retry from CXL controller.\n");
    owner->responseStalled = false; // Clear stall flag

    // Try sending the request that was previously stalled
    if (owner->respondingRequest) {
        DecompressionRequest* req_to_send = owner->respondingRequest;
        owner->respondingRequest = nullptr; // Clear it before trying to send

        // The packet to send is req_to_send->pkt, which should have response data
        bool success = sendTimingResp(req_to_send->pkt);
        if (success) {
            DPRINTF(DecompEngine, "Successfully resent stalled response for req %p (addr %#x).\n",
                    req_to_send, req_to_send->pkt->getAddr());
            delete req_to_send; // Clean up the DecompressionRequest object
        } else {
            owner->responseStalled = true; // Still stalled
            owner->respondingRequest = req_to_send; // Put it back
            DPRINTF(DecompEngine, "CXL port still busy after retry for req %p (addr %#x).\n",
                    req_to_send, req_to_send->pkt->getAddr());
            return; // Exit if still stalled
        }
    }

    // Try sending other completed requests from the queue
    while (!owner->responseStalled && !owner->completed_queue.empty()) {
        DecompressionRequest* next_req = owner->completed_queue.front();
        // owner->completed_queue.pop(); // Pop only after successful send or re-queueing

        DPRINTF(DecompEngine, "Attempting to send next completed req %p (addr %#x) from queue.\n",
                next_req, next_req->pkt->getAddr());
        bool next_success = sendTimingResp(next_req->pkt);
        if (next_success) {
            owner->completed_queue.pop(); // Successfully sent
            DPRINTF(DecompEngine, "Successfully sent next completed req %p from queue.\n", next_req);
            delete next_req;
        } else {
            owner->responseStalled = true;
            owner->respondingRequest = next_req; // This is now the stalled request
            // Do not pop from completed_queue yet, it's effectively stalled. Or pop and set as respondingRequest.
            // For simplicity, let's assume respondingRequest takes precedence over queue.
            // If it's set as respondingRequest, it should be removed from queue to avoid double processing.
            // However, the current logic implies respondingRequest is only for the *first* one that stalled.
            // Let's refine: if send fails, it becomes the new respondingRequest.
            DPRINTF(DecompEngine, "CXL port stalled again for req %p from completed_queue.\n", next_req);
            break; // Stop processing queue
        }
    }

    // If CXL port is free, try to schedule more decompressions
    if (!owner->responseStalled) {
        owner->tryScheduleDecompression();
    }
    // And try to process CXL-side request retries
    if (!owner->cxlReqStalled) {
        trySendRetries();
    }
}

void
DecompressionEngine::CXLSidePort::trySendRetries() {
    while (!owner->cxlRequestRetryQueue.empty()) {
        PacketPtr pkt_to_retry = owner->cxlRequestRetryQueue.front();
        // Attempt to re-process this packet by calling recvTimingReq again
        // Need to pop it first to avoid potential infinite loop if recvTimingReq itself queues it back
        owner->cxlRequestRetryQueue.pop();
        DPRINTF(DecompEngine, "Retrying CXL-side request for addr %#x.\n", pkt_to_retry->getAddr());
        if (recvTimingReq(pkt_to_retry)) { // This will re-evaluate cxlReqStalled
            // Successfully re-queued or processed
            if (owner->cxlReqStalled) { // If recvTimingReq decided to stall again
                DPRINTF(DecompEngine, "CXL-side still stalled after retry for addr %#x, request re-queued internally by recvTimingReq.\n", pkt_to_retry->getAddr());
                // pkt_to_retry might have been pushed back by recvTimingReq if it's still stalled.
                // Or it might have been processed.
                return; // Stop retrying if stalled again
            }
        } else {
            // This case should ideally not happen if recvTimingReq always returns true.
            // If it can return false, then we need to put it back and mark stalled.
            owner->cxlRequestRetryQueue.push(pkt_to_retry); // Put it back if not accepted
            owner->cxlReqStalled = true;
            DPRINTF(DecompEngine, "Retry for CXL-side request addr %#x failed to be accepted by recvTimingReq.\n", pkt_to_retry->getAddr());
            return;
        }
    }
    // If queue is empty, we are no longer stalled for CXL-side new requests
    owner->cxlReqStalled = false;
    DPRINTF(DecompEngine, "CXL-side request retry queue empty, no longer cxlReqStalled.\n");
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
        if (pair.second->pkt && pkt->trySatisfyFunctional(pair.second->pkt)) { // Check if chunk's pkt exists
            return;
        }
         if (pair.second->respPkt && pkt->trySatisfyFunctional(pair.second->respPkt)) { // Check if chunk's respPkt exists
            return;
        }
    }

    std::queue<DecompressionRequest*> temp_q;
    bool found = false;

    // Check memSendCandidateQueue (formerly memoryRetryQueue)
    // This requires careful handling as memSendCandidateQueue is also used for active sends.
    // For functional access, we are checking if the data *would* be satisfied.
    // A simple iteration might be okay if modifications are not made.
    // Create a temporary copy for iteration if modification during iteration is a concern.
    // However, trySatisfyFunctional doesn't modify the queue structure.
    // We need to iterate over a copy or be careful if trySatisfyFunctional could trigger state changes.
    // For now, let's assume simple iteration is fine for functional check.
    // If memSendCandidateQueue can be modified by trySatisfyFunctional indirectly, this needs rework.
    // A safer approach for queues is to pop, check, and push to a temporary queue, then restore.
    std::queue<DecompressionRequest*> current_candidates = owner->memSendCandidateQueue; // Make a copy for iteration
    while(!current_candidates.empty()){
        DecompressionRequest* req = current_candidates.front();
        current_candidates.pop(); // Iterate through the copy
        if(!found && req->pkt && pkt->trySatisfyFunctional(req->pkt)) found = true;
        if(!found && req->respPkt && pkt->trySatisfyFunctional(req->respPkt)) found = true;
        // Do not push back to temp_q here as we are iterating a copy
    }
    // owner->memSendCandidateQueue remains unchanged by this block.
    if(found) return;


    // Check decompression_queue
    while(!owner->decompression_queue.empty()){
        DecompressionRequest* req = owner->decompression_queue.front();
        owner->decompression_queue.pop();
        if(!found && req->pkt && pkt->trySatisfyFunctional(req->pkt)) found = true;
        if(!found && req->respPkt && pkt->trySatisfyFunctional(req->respPkt)) found = true;
        temp_q.push(req);
    }
    owner->decompression_queue = std::move(temp_q);
    if(found) return;

    // Check completed_queue
    while(!owner->completed_queue.empty()){
        DecompressionRequest* req = owner->completed_queue.front();
        owner->completed_queue.pop();
        if(!found && req->pkt && pkt->trySatisfyFunctional(req->pkt)) found = true;
        if(!found && req->respPkt && pkt->trySatisfyFunctional(req->respPkt)) found = true;
        temp_q.push(req);
    }
    owner->completed_queue = std::move(temp_q);
    if(found) return;

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
    // Try to schedule next send from the candidate queue
    owner->tryScheduleNextMemSend();
}

void
DecompressionEngine::handleResponse(PacketPtr memRespPkt)
{
    Addr respAddr = memRespPkt->getAddr();
    auto it = pendingRequests.find(respAddr);

    if (it == pendingRequests.end()) {
        warn("Received memory response for addr %#x, but no matching pending request found. Ignoring.\n", respAddr);
        delete memRespPkt;
        return;
    }

    DecompressionRequest* req = it->second; // This is the DecompressionRequest for the chunk or non-chunked item
    pendingRequests.erase(it);

    if (req->isChunk) {
        DecompressionRequest* parentReq = req->parentRequest;
        assert(parentReq);
        DPRINTF(DecompEngine, "ParentReq %p: Received response for chunk %u (addr %#x, memRespPkt %p, size %u, data %p).\n",
                parentReq, req->chunkIndex, respAddr, memRespPkt, memRespPkt->getSize(), memRespPkt->hasData() ? memRespPkt->getConstPtr<void>() : nullptr);

        if (memRespPkt->hasData() && parentReq->responseData) {
            unsigned offset = req->chunkIndex * cache_line_size;

            // Check if offset is within the parent request size (important safety check)
            if (offset >= parentReq->pkt->getSize()) {
                warn("ParentReq %p: Chunk offset %u exceeds parent request size %u for chunk %u. Skipping copy.",
                     parentReq, offset, parentReq->pkt->getSize(), req->chunkIndex);
            } else {
                // Safely calculate copy size
                size_t remainingSize = parentReq->pkt->getSize() - offset;
                size_t copySize = std::min((size_t)memRespPkt->getSize(), remainingSize);

                if (copySize > 0) {
                    std::memcpy(parentReq->responseData + offset,
                               memRespPkt->getConstPtr<uint8_t>(),
                               copySize);
                    DPRINTF(DecompEngine, "ParentReq %p: Copied %u bytes from chunk %u to offset %u.\n",
                            parentReq, (unsigned int)copySize, req->chunkIndex, offset);
                } else {
                    warn("ParentReq %p: Calculated copy size is 0 for chunk %u at offset %u.",
                         parentReq, req->chunkIndex, offset);
                }
            }
        } else {
            if (!memRespPkt->hasData()) {
                warn("ParentReq %p: Memory response packet has no data for chunk %u.",
                     parentReq, req->chunkIndex);
            }
            if (!parentReq->responseData) {
                warn("ParentReq %p: responseData is null for chunk %u.",
                     parentReq, req->chunkIndex);
            }
        }

        // Safe cleanup - first backup important values
        unsigned chunkIndex = req->chunkIndex;
        unsigned totalChunks = parentReq->totalChunks;

        DPRINTF(DecompEngine, "ParentReq %p: Cleaning up chunk %u resources (memRespPkt %p, req->pkt %p, req %p).\n",
                parentReq, chunkIndex, memRespPkt, req->pkt, req);

        // Find this chunk in parent's vector and mark it as nullptr
        // This is crucial to avoid dangling pointers in the parent's vector
        bool found = false;
        for (size_t i = 0; i < parentReq->chunkRequests.size(); i++) {
            if (parentReq->chunkRequests[i] == req) {
                DPRINTF(DecompEngine, "ParentReq %p: Marking chunk %u (req %p) as nullptr in parent's vector at index %zu.\n",
                        parentReq, chunkIndex, req, i);
                parentReq->chunkRequests[i] = nullptr; // Mark as deleted in the parent's vector
                found = true;
                break;
            }
        }

        if (!found) {
            DPRINTF(DecompEngine, "WARNING: ParentReq %p: Could not find chunk %u (req %p) in parent's vector.\n",
                    parentReq, chunkIndex, req);
        }

        // Check if memRespPkt and req->pkt are the same pointer
        // This can happen if the memory system returns the same packet that was sent
        bool same_packet = (memRespPkt == req->pkt);

        if (same_packet) {
            DPRINTF(DecompEngine, "ParentReq %p: memRespPkt and req->pkt are the same (%p). Will only delete once.\n",
                    parentReq, memRespPkt);
            // Only delete the packet once
            delete memRespPkt;
            req->pkt = nullptr; // Set to nullptr to avoid double deletion
        } else {
            // Normal case - different pointers
            if (memRespPkt) {
                delete memRespPkt;
            }

            if (req->pkt) {
                delete req->pkt;
                req->pkt = nullptr;
            }
        }

        // Delete the chunk request object (safe now that packet pointers are nullified)
        delete req;

        // Update parent's completed chunks count
        parentReq->completedChunks++;

        // Log chunk completion status
        DPRINTF(DecompEngine, "ParentReq %p: Completed chunk %u/%u.\n",
                parentReq, parentReq->completedChunks, totalChunks);

        if (parentReq->completedChunks == totalChunks) {
            DPRINTF(DecompEngine, "ParentReq %p: All %u chunks completed for addr %#x. Original CXL Pkt: %p (size %u, data %p)\n",
                    parentReq, totalChunks, parentReq->pkt->getAddr(), parentReq->pkt, parentReq->pkt->getSize(), parentReq->pkt->hasData() ? parentReq->pkt->getConstPtr<void>() : nullptr);

            // Data is now in parentReq->responseData. Copy it to parentReq->pkt (original CXLController packet)
            if (parentReq->pkt->hasData() && parentReq->responseData) { // Ensure original packet can hold data
                 size_t finalCopySize = parentReq->pkt->getSize(); // Assuming full size
                 std::memcpy(parentReq->pkt->getPtr<uint8_t>(), parentReq->responseData, finalCopySize);
                 DPRINTF(DecompEngine, "ParentReq %p: Copied %u bytes from aggregated buffer to final response packet %p.\n",
                        parentReq, (unsigned int)finalCopySize, parentReq->pkt);
            } else if (!parentReq->pkt->hasData() && parentReq->responseData && parentReq->pkt->isRead()) {
                // This case might happen if the original packet from CXL controller didn't allocate data
                // (e.g., if it was a ReadReq expecting data in response)
                warn("ParentReq %p: Original CXL packet %p has no data buffer, cannot copy aggregated data for read response.", parentReq, parentReq->pkt);
            }

            // Clean up the response data buffer
            if (parentReq->responseData) {
                delete[] parentReq->responseData;
                parentReq->responseData = nullptr;
            }

            // Make sure the command is a response and data flags are set
            if (parentReq->pkt->isRead()) {
                if (!parentReq->pkt->isResponse()) { // Check before calling
                    parentReq->pkt->makeResponse();
                }
                DPRINTF(DecompEngine, "ParentReq %p: CXL Pkt %p is now a ReadResp. Cmd: %s, HasData: %d, Size: %u\n",
                        parentReq, parentReq->pkt, parentReq->pkt->cmdString(), parentReq->pkt->hasData(), parentReq->pkt->getSize());
            }

            // Queue the parent request for decompression
            decompression_queue.push(parentReq);
            tryScheduleDecompression();
        }
    } else { // Not a chunk, i.e., a direct request (write or small read)
        DPRINTF(DecompEngine, "Received response for non-chunked req %p (addr %#x, CXL Pkt: %p, MemRespPkt: %p).\n",
                req, respAddr, req->pkt, memRespPkt);

        assert(req->pkt == memRespPkt && "For non-chunked requests, req->pkt should be the same as memRespPkt");

        if (req->pkt->isRead()) { // Small read request
            DPRINTF(DecompEngine, "Non-chunked Read: CXL Pkt %p (MemRespPkt) received from memory. Cmd: %s, Size: %u, HasData: %d, IsResp: %d, NeedsResp: %d\n",
                    req->pkt, req->pkt->cmdString(), req->pkt->getSize(), req->pkt->hasData(), req->pkt->isResponse(), req->pkt->needsResponse());

            if (!req->pkt->isResponse()) {
                if (req->pkt->needsResponse()) {
                    req->pkt->makeResponse();
                } else {
                    warn("Non-chunked Read: CXL Pkt %p is not a response but claims not to need one. Cmd: %s. Attempting to make it a response if not an error.",
                          req->pkt, req->pkt->cmdString());
                    if (!req->pkt->isError()) {
                        // If it's not an error and not a response, but needsResponse() is false,
                        // this is an unusual state. Forcing it to be a response.
                        // makeResponse() should handle setting the correct command and flags.
                        req->pkt->makeResponse();
                        // If makeResponse() doesn't set data flags correctly when data is present,
                        // that's an issue in makeResponse() or how data was set.
                        if (req->pkt->hasData() && req->pkt->cmd != MemCmd::ReadResp) { // CHECK CMD
                             warn("Non-chunked Read: Forced Pkt %p to response, but it's not ReadResp. Cmd: %s. Data flags might be incorrect.",
                                   req->pkt, req->pkt->cmdString());
                        }
                    }
                }
            }
            DPRINTF(DecompEngine, "Non-chunked Read: CXL Pkt %p processed. Cmd: %s, HasData: %d, Size: %u\n",
                    req->pkt, req->pkt->cmdString(), req->pkt->hasData(), req->pkt->getSize());

            decompression_queue.push(req);
            tryScheduleDecompression();
        } else { // Write request response
            DPRINTF(DecompEngine, "Non-chunked Write: Forwarding memRespPkt %p (Cmd: %s) to CXL controller for req %p (CXL Pkt %p).\n",
                    memRespPkt, memRespPkt->cmdString(), req, req->pkt);

            bool success = cxlPort.sendTimingResp(memRespPkt);
            if (success) {
                DPRINTF(DecompEngine, "Successfully forwarded write response for req %p (addr %#x).\n", req, respAddr);
                // req->pkt is the original CXLController packet, don't delete.
                // memRespPkt is deleted by the port or callee if successful.
                delete req; // Delete the DecompressionRequest wrapper
            } else {
                responseStalled = true;
                // req->pkt is the original request. We need to associate memRespPkt with req for retry.
                req->respPkt = memRespPkt; // Store the actual response packet for retry
                respondingRequest = req;
                DPRINTF(DecompEngine, "CXL port stalled, delaying write response for req %p (addr %#x). Stored memRespPkt %p in req->respPkt.\n",
                        req, respAddr, memRespPkt);
            }
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
    // Use fully qualified name for the nested class DecompressionEvent
    DecompressionEngine::DecompressionEvent* event = new DecompressionEngine::DecompressionEvent(this, req);
    schedule(event, completionTime);

    DPRINTF(DecompEngine, "Scheduled decompression for req %p (addr %#x) to complete at tick %llu (latency: %llu ps)\n",
           req, req->pkt->getAddr(), completionTime, decompTime);
}

void
DecompressionEngine::completeDecompression(DecompressionRequest* req) // req is a parent request or non-chunked read
{
    DPRINTF(DecompEngine, "Decompression complete for req %p (addr %#x, CXL Pkt %p).\n",
            req, req->pkt->getAddr(), req->pkt);
    assert(active_decompressions > 0);
    active_decompressions--;
    req->readyToRespond = true;

    // The packet to send back is req->pkt, which now contains the (decompressed) data.
    // Ensure it's marked as a response and data flags are correct.
    // For reads, makeResponse() should have been called in handleResponse.
    // For writes, this path should ideally not be taken if they are acked immediately.
    // If a write response was queued for decompression (unlikely), its pkt cmd should be WriteResp.

    if (req->pkt->isRead() && !req->pkt->isResponse()) {
        // This implies makeResponse() was not called or was reverted.
        warn("DecompComplete: Read packet %p for req %p is not a response. Making it one.", req->pkt, req);
        req->pkt->makeResponse();
    } else if (req->pkt->isWrite() && !req->pkt->isResponse()) {
        // This is more unusual for writes that went through decompression.
        warn("DecompComplete: Write packet %p for req %p is not a response. Making it one (WriteResp).", req->pkt, req);
        req->pkt->makeResponse(); // This will make it a WriteResp
    }

    DPRINTF(DecompEngine, "DecompComplete: Sending CXL Pkt %p (Cmd: %s, HasData: %d, Size: %u) for req %p.\n",
            req->pkt, req->pkt->cmdString(), req->pkt->hasData(), req->pkt->getSize(), req);


    if (!responseStalled) {
        // For writes that were stalled and had their response in req->respPkt:
        // If req->respPkt is not null, it means this is a stalled write response.
        PacketPtr pkt_to_send = req->respPkt ? req->respPkt : req->pkt;

        bool success = cxlPort.sendTimingResp(pkt_to_send);
        if (success) {
            DPRINTF(DecompEngine, "Successfully sent final response for req %p (using pkt %p).\n", req, pkt_to_send);

            // req->pkt is owned by CXLController if it's the original request packet.
            // req->respPkt (if it was a memory response for a write) is now sent and can be deleted.
            if (pkt_to_send == req->respPkt) {
                delete req->respPkt;
                req->respPkt = nullptr;
            }

            delete req; // Delete the DecompressionRequest wrapper.
        } else {
            responseStalled = true;
            respondingRequest = req; // req still holds pkt_to_send (either req->pkt or req->respPkt)
            DPRINTF(DecompEngine, "CXL port stalled, delaying final response for req %p (using pkt %p).\n", req, pkt_to_send);
        }
    } else {
        completed_queue.push(req);
        DPRINTF(DecompEngine, "CXL port stalled, adding completed req %p to completed_queue.\n", req);
    }
    tryScheduleDecompression();
}

// ADDED: Method to try scheduling next memory send
void
DecompressionEngine::tryScheduleNextMemSend() {
    if (memSendCandidateQueue.empty()) {
        DPRINTF(DecompEngine, "tryScheduleNextMemSend: Queue empty.\n");
        return;
    }
    if (memSendEventScheduled) {
        DPRINTF(DecompEngine, "tryScheduleNextMemSend: Event already scheduled.\n");
        return;
    }
    if (memoryStalled) {
        DPRINTF(DecompEngine, "tryScheduleNextMemSend: Memory port stalled.\n");
        return;
    }

    Tick schedule_at = std::max(clockEdge(), nextMemSendAvailableAt);

    DPRINTF(DecompEngine, "tryScheduleNextMemSend: Scheduling MemSendEvent at %llu (curTick %llu, nextAvailable %llu).\n",
            schedule_at, curTick(), nextMemSendAvailableAt);

    schedule(&memSendEvent, schedule_at);
    memSendEventScheduled = true;
}

} // namespace gem5
