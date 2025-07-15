#include "cxl_objects/decompression_engine.hh"

#include "base/trace.hh"
#include "debug/DecompEngine.hh"
#include "sim/system.hh"
#include "mem/packet_access.hh" // For packet data access

namespace gem5
{

const unsigned DecompressionEngine::NUM_MEMORY_PORTS;

// ChunkSendEvent process method
void
DecompressionEngine::ChunkSendEvent::process() {
    engine->sendNextChunk(parentRequest, nextChunkIndex);
}

// ScheduleMemSendEvent process method
void
DecompressionEngine::ScheduleMemSendEvent::process() {
    engine->memSendEventScheduled = false; // Event is now processing

    if (engine->memSendCandidateQueue.empty()) {
        DPRINTF(DecompEngine, "MemSendEvent: Queue is empty, nothing to send.\n");
        return;
    }

    DecompressionRequest* req_to_send = engine->memSendCandidateQueue.front();
    // Packet must exist
    assert(req_to_send && req_to_send->pkt);

    DPRINTF(DecompEngine, "MemSendEvent: Attempting to send req %p (pkt addr %#x, type: %s) to memory.\n",
           req_to_send, req_to_send->pkt->getAddr(), req_to_send->isChunk ? "Chunk" : "Parent");

    // Select port based on address interleaving
    unsigned portIndex = engine->selectMemoryPort(req_to_send->pkt->getAddr());

    // Check if the selected port is stalled
    if (engine->memoryPortStalled[portIndex]) {
        // Add to the port's retry queue instead of trying other ports
        engine->memPortRetryQueues[portIndex].push(req_to_send);
        engine->memSendCandidateQueue.pop();

        DPRINTF(DecompEngine, "MemSendEvent: Memory port %u is stalled for addr %#x, added to port's retry queue.\n",
                portIndex, req_to_send->pkt->getAddr());

        // Try to schedule the next send for other addresses
        engine->tryScheduleNextMemSend();
        return;
    }

    bool success = engine->sendToMemoryPort(req_to_send, portIndex);

    if (success) {
        engine->memSendCandidateQueue.pop();
        if (req_to_send->isChunk) {
            engine->pendingRequests[req_to_send->pkt->getAddr()] = req_to_send;
        } else {
            warn("MemSendEvent: Non-chunk request %p sent to memory and added to pendingRequests. Review if this is intended.", req_to_send);
            engine->pendingRequests[req_to_send->pkt->getAddr()] = req_to_send;
        }
        engine->nextMemSendAvailableAt = engine->clockEdge() + engine->interMemoryRequestDelay;
        DPRINTF(DecompEngine, "MemSendEvent: Successfully sent req %p to memory port %d (interleaved). Next send available at %llu.\n",
                req_to_send, portIndex, engine->nextMemSendAvailableAt);
    } else {
        // This port is now stalled, add request to port's retry queue
        engine->memoryPortStalled[portIndex] = true;
        engine->memPortRetryQueues[portIndex].push(req_to_send);
        engine->memSendCandidateQueue.pop();

        DPRINTF(DecompEngine, "MemSendEvent: Memory port %d busy for req %p, added to port's retry queue.\n",
                portIndex, req_to_send);
    }

    // Try to schedule the next send if conditions allow
    engine->tryScheduleNextMemSend();
}

// Modify the method to select memory port based on address interleaving only
unsigned
DecompressionEngine::selectMemoryPort(Addr addr) {
    if (addr >= 0x800000000) {
        warn("Address %#x is outside of decompression memory range (0-32GB). Using port 0.\n", addr);
        return 0;
    }

    // Extract the interleaved bits from the address
    unsigned interleavedBits = (addr >> interleavingLowBit) & ((1 << interleavingBits) - 1);

    // 포트 번호가 유효한지 검증
    if (interleavedBits >= NUM_MEMORY_PORTS) {
        warn("Calculated port %u for address %#x is invalid (max port: %u). Using port %u instead.\n",
             interleavedBits, addr, NUM_MEMORY_PORTS - 1, interleavedBits % NUM_MEMORY_PORTS);
        // 유효하지 않은 경우 모듈로 연산으로 안전한 값 반환
        return interleavedBits % NUM_MEMORY_PORTS;
    }

    DPRINTF(DecompEngine, "Selected port %u for address %#x based on interleaving (bits %u-%u = %u)\n",
            interleavedBits, addr, interleavingLowBit,
            interleavingLowBit + interleavingBits - 1, interleavedBits);

    return interleavedBits;
}

// Add method to process retry queue for a specific port
void
DecompressionEngine::tryMemPortRetry(unsigned portIndex) {
    if (memPortRetryQueues[portIndex].empty()) {
        DPRINTF(DecompEngine, "No requests in retry queue for port %u\n", portIndex);
        return;
    }

    // Try to send the first request in the queue
    DecompressionRequest* req = memPortRetryQueues[portIndex].front();

    DPRINTF(DecompEngine, "Trying to send request %p to port %u from retry queue\n",
            req, portIndex);

    bool success = sendToMemoryPort(req, portIndex);

    if (success) {
        // Remove from retry queue
        memPortRetryQueues[portIndex].pop();

        // Add to pending requests
        if (req->isChunk) {
            pendingRequests[req->pkt->getAddr()] = req;
        } else {
            warn("MemPortRetry: Non-chunk request %p sent to memory and added to pendingRequests.", req);
            pendingRequests[req->pkt->getAddr()] = req;
        }

        DPRINTF(DecompEngine, "Successfully sent req %p to memory port %u from retry queue\n",
                req, portIndex);

        // Try more from the same queue if available
        if (!memPortRetryQueues[portIndex].empty()) {
            tryMemPortRetry(portIndex);
        }
    } else {
        // Port is still stalled
        memoryPortStalled[portIndex] = true;
        DPRINTF(DecompEngine, "Port %u still stalled for retry request %p\n",
                portIndex, req);
    }
}

// Update constructor to initialize ports as members
DecompressionEngine::DecompressionEngine(const DecompressionEngineParams &params)
    : ClockedObject(params),
      cxlPort(name() + ".cxl_side_port", this),
      memPort0(name() + ".mem_side_port_0", this, 0),
      memPort1(name() + ".mem_side_port_1", this, 1),
      memPort2(name() + ".mem_side_port_2", this, 2),
      memPort3(name() + ".mem_side_port_3", this, 3),
      memoryPortStalled{false, false, false, false}, // 4개 포트에 대한 stalled 플래그 초기화
      nextMemoryPortIndex(0),
      interleavingLowBit(params.interleaving_low_bit),
      interleavingBits(params.interleaving_bits),
      responseStalled(false),
      respondingRequest(nullptr),
      block_size(params.block_size),
      cache_line_size(params.cache_line_size),
      num_engines(params.num_engines),
      active_decompressions(0),
      chunkSendDelay(params.chunk_send_delay_ticks),
      cxlReqStalled(false),
      nextMemSendAvailableAt(0),
      interMemoryRequestDelay(params.inter_memory_request_delay_ticks),
      memSendEvent(this),
      memSendEventScheduled(false)
{
    fatal_if(num_engines == 0, "DecompressionEngine must have at least one engine.");
    fatal_if(NUM_MEMORY_PORTS != (1U << interleavingBits),
             "Number of memory ports (%u) must match the interleaving width (2^%u = %u)",
             NUM_MEMORY_PORTS, interleavingBits, (1U << interleavingBits));

    // memPorts 배열에 포인터 할당
    memPorts[0] = &memPort0;
    memPorts[1] = &memPort1;
    memPorts[2] = &memPort2;
    memPorts[3] = &memPort3;

    DPRINTF(DecompEngine, "Initialized with %u decompression engines and %u memory ports. "
            "Using address interleaving: bits %u-%u. "
            "Each memory port has its own retry queue.\n",
            num_engines, NUM_MEMORY_PORTS, interleavingLowBit,
            interleavingLowBit + interleavingBits - 1);
}

DecompressionEngine::~DecompressionEngine()
{
    DPRINTF(DecompEngine, "DecompressionEngine destructor called. Cleaning up resources.\n");

    // Clear pendingRequests (these are chunk requests)
    for (auto& pair : pendingRequests) {
        DecompressionRequest* req = pair.second;
        if (req) {
            // Chunk's pkt is created by DecompressionEngine
            delete req->pkt; // req->pkt is the chunk's request to memory
            delete req->respPkt; // respPkt is the chunk's response from memory
            delete req; // Delete the DecompressionRequest object for the chunk
        }
    }
    pendingRequests.clear();

    // Clear memSendCandidateQueue (these are chunk requests waiting to be sent)
    while (!memSendCandidateQueue.empty()) {
        DecompressionRequest* req = memSendCandidateQueue.front();
        memSendCandidateQueue.pop();
        if (req) {
            assert(req->isChunk); // Should only contain chunks now
            delete req->pkt;
            // respPkt should be null here
            delete req;
        }
    }

    // Clear decompression_queue (parent requests ready for decompression)
    while (!decompression_queue.empty()) {
        DecompressionRequest* req = decompression_queue.front();
        decompression_queue.pop();
        // req->pkt is from CXLController (don't delete).
        // req->respPkt is not typically used for parent requests in this queue.
        // DecompressionRequest destructor handles responseData and its chunkRequests.
        delete req;
    }

    // Clear completed_queue (parent requests or readiness updates waiting to send to CXL)
    while (!completed_queue.empty()) {
        DecompressionRequest* req = completed_queue.front();
        completed_queue.pop();
        // If it's a readiness update, req->pkt is created by DecompEngine.
        if (req->isReadinessUpdate) {
            delete req->pkt;
        }
        // If parent, pkt is from CXL. respPkt not used.
        delete req;
    }

    // Clear memParentRetryQueue (should not be used anymore)
     while (!memParentRetryQueue.empty()) {
        DecompressionRequest* req = memParentRetryQueue.front();
        memParentRetryQueue.pop();
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
        // If it's a readiness update, its packet was created by DecompEngine
        if (respondingRequest->isReadinessUpdate) {
            delete respondingRequest->pkt;
        }
        // If it's a parent request, its pkt is from CXLController (don't delete).
        // Its respPkt is not used for parent requests in this context.
        delete respondingRequest;
        respondingRequest = nullptr;
    }

    // Clean up per-port retry queues
    for (auto& queue : memPortRetryQueues) {
        while (!queue.empty()) {
            DecompressionRequest* req = queue.front();
            queue.pop();
            if (req) {
                if (req->pkt) {
                    delete req->pkt;
                }
                delete req;
            }
        }
    }
}

Port &
DecompressionEngine::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cxl_side_port") {
        return cxlPort;
    } else if (if_name == "mem_side_port_0") {
        return memPort0;
    } else if (if_name == "mem_side_port_1") {
        return memPort1;
    } else if (if_name == "mem_side_port_2") {
        return memPort2;
    } else if (if_name == "mem_side_port_3") {
        return memPort3;
    } else {
        return ClockedObject::getPort(if_name, idx);
    }
}

bool
DecompressionEngine::CXLSidePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(DecompEngine, "Received large read request for addr %#x, size %u from CXL controller.\n",
            pkt->getAddr(), pkt->getSize());

    if (owner->cxlReqStalled) {
        DPRINTF(DecompEngine, "CXL side port stalled, queueing request for addr %#x.\n", pkt->getAddr());
        owner->cxlRequestRetryQueue.push(pkt);
        return true; // Accepted into retry queue
    }

    DecompressionRequest* parentReq = new DecompressionRequest(pkt, curTick());
    parentReq->isChunk = false; // This is a parent request

    // Extract decompression latency if available in packet data
    if (pkt->hasData() && pkt->getSize() >= sizeof(double) * 2) {
        const double* dataPtr = reinterpret_cast<const double*>(pkt->getConstPtr<uint8_t>());
        parentReq->decompLatency_ns = dataPtr[1]; // Get decompression latency from second double
        DPRINTF(DecompEngine, "Extracted decompLatency=%.2f ns from packet data\n",
                parentReq->decompLatency_ns);
    }

    // Calculate the number of chunks
    if (pkt->getSize() <= owner->cache_line_size) {
        // If request size is less than or equal to cache line size, just use one chunk
        parentReq->totalChunks = 1;
        DPRINTF(DecompEngine, "Small request (%u bytes) - using single chunk\n", pkt->getSize());
    } else {
        // Otherwise calculate multiple chunks as before
        parentReq->totalChunks = pkt->getSize() / owner->cache_line_size;
        if (pkt->getSize() % owner->cache_line_size != 0) {
            parentReq->totalChunks++;
        }
    }

    // Ensure at least one chunk if size > 0 and totalChunks ended up 0
    if (pkt->getSize() > 0 && parentReq->totalChunks == 0) parentReq->totalChunks = 1;


    parentReq->responseData = new char[pkt->getSize()]; // Buffer for aggregated data
    std::memset(parentReq->responseData, 0, pkt->getSize());

    DPRINTF(DecompEngine, "Splitting read request for addr %#x (size %u) into %u chunks.\n",
            pkt->getAddr(), pkt->getSize(), parentReq->totalChunks);

    // Send the first chunk
    owner->sendNextChunk(parentReq, 0);

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
        // This can happen if totalChunks was calculated slightly off for a zero-size request or edge case.
        // Or if fullSize itself is 0.
        DPRINTF(DecompEngine, "Calculated chunk size is 0 for chunkIndex %u, offset %u, fullSize %u. Skipping chunk.\n",
             chunkIndex, offset, fullSize);
        // If this was the last chunk, we might need to finalize the parent.
        // However, if fullSize is 0, totalChunks should also be 0.
        // If fullSize > 0, chunkSize should not be 0 unless offset >= fullSize.
        if (offset >= fullSize && chunkIndex < parentReq->totalChunks) {
             // This implies an issue with totalChunks calculation or call sequence.
             warn("Offset %u >= fullSize %u but chunkIndex %u < totalChunks %u. Finalizing parent early.",
                  offset, fullSize, chunkIndex, parentReq->totalChunks);
             // To prevent infinite loops or stalls, consider this the end of chunks for this parent.
             parentReq->completedChunks = parentReq->totalChunks; // Mark all as "done"
             // Then proceed to handleResponse logic for parent completion.
             // This is a recovery path.
             if (parentReq->completedChunks == parentReq->totalChunks) {
                 // Simplified parent completion logic here, normally in handleResponse
                 DPRINTF(DecompEngine, "ParentReq %p: All %u chunks (effectively) completed due to zero-size chunk. Queuing for decompression.\n",
                         parentReq, parentReq->totalChunks);
                 // Copy data if any was aggregated (likely none in this path)
                 if (parentReq->pkt->hasData() && parentReq->responseData) { // Check pkt can hold data
                     std::memcpy(parentReq->pkt->getPtr<uint8_t>(), parentReq->responseData, parentReq->pkt->getSize());
                 }
                 delete[] parentReq->responseData;
                 parentReq->responseData = nullptr;
                 parentReq->pkt->makeResponse(); // Ensure it's a response

                 decompression_queue.push(parentReq);
                 tryScheduleDecompression();
             }
        }
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
        // Schedule based on clock period for cycle-based delays
        schedule(event, clockEdge(Cycles(chunkSendDelay)));
    }
}

void
DecompressionEngine::CXLSidePort::recvRespRetry()
{
    DPRINTF(DecompEngine, "Received response retry from CXL controller.\n");
    owner->responseStalled = false; // Clear stall flag

    // Try sending the request that was previously stalled
    DecompressionRequest* req_to_send_stalled = owner->respondingRequest;
    if (req_to_send_stalled) {
        owner->respondingRequest = nullptr; // Clear it before trying to send

        // Ensure packet is a response before sending
        // This is crucial for readiness updates and final parent responses
        if (req_to_send_stalled->pkt->isRequest()) {
             req_to_send_stalled->pkt->makeResponse();
        }

        bool success = sendTimingResp(req_to_send_stalled->pkt);
        if (success) {
            DPRINTF(DecompEngine, "Successfully resent stalled response for req %p (addr %#x).\n",
                    req_to_send_stalled, req_to_send_stalled->pkt->getAddr());
            // If it was a readiness update, its packet was created by DecompressionEngine and needs deletion.
            if (req_to_send_stalled->isReadinessUpdate) {
                delete req_to_send_stalled->pkt;
            }
            // Parent request's original packet (req_to_send_stalled->pkt) is owned by CXLController.
            delete req_to_send_stalled; // Clean up the DecompressionRequest object
        } else {
            owner->responseStalled = true; // Still stalled
            owner->respondingRequest = req_to_send_stalled; // Put it back
            DPRINTF(DecompEngine, "CXL port still busy after retry for req %p (addr %#x).\n",
                    req_to_send_stalled, req_to_send_stalled->pkt->getAddr());
            return; // Exit if still stalled
        }
    }

    // Try sending other completed requests from the queue
    while (!owner->responseStalled && !owner->completed_queue.empty()) {
        DecompressionRequest* next_req = owner->completed_queue.front();
        // owner->completed_queue.pop(); // Pop only after successful send or re-queueing

        // Ensure packet is a response before sending
        if (next_req->pkt->isRequest()) {
            next_req->pkt->makeResponse();
        }

        DPRINTF(DecompEngine, "Attempting to send next completed req %p (addr %#x, type: %s) from queue.\n",
                next_req, next_req->pkt->getAddr(), next_req->isReadinessUpdate ? "ReadinessUpdate" : "ParentResponse");
        bool next_success = sendTimingResp(next_req->pkt);
        if (next_success) {
            owner->completed_queue.pop(); // Successfully sent
            DPRINTF(DecompEngine, "Successfully sent next completed req %p from queue.\n", next_req);
            if (next_req->isReadinessUpdate) {
                delete next_req->pkt;
            }
            delete next_req;
        } else {
            owner->responseStalled = true;
            owner->respondingRequest = next_req; // This is now the stalled request
            // Do not pop from completed_queue yet, it's effectively stalled.
            DPRINTF(DecompEngine, "CXL port stalled again for req %p from completed_queue.\n", next_req);
            break; // Stop processing queue
        }
    }

    // If CXL port is free, try to schedule more decompressions
    if (!owner->responseStalled) {
        owner->tryScheduleDecompression();
    }
    // And try to process CXL-side request retries
    // cxlReqStalled is set by recvTimingReq if it cannot accept new requests.
    // If it's false, we can try to send from the retry queue.
    if (!owner->cxlReqStalled) {
        trySendRetries();
    }
}

void
DecompressionEngine::CXLSidePort::trySendRetries() {
    while (!owner->cxlRequestRetryQueue.empty()) {
        // If recvTimingReq itself decides it's stalled (e.g., internal resource limit),
        // it will set owner->cxlReqStalled = true and re-queue the packet.
        // So, we should check owner->cxlReqStalled before attempting to process.
        if (owner->cxlReqStalled) {
             DPRINTF(DecompEngine, "CXL-side retry paused because cxlReqStalled is true.\n");
             return; // Stop retrying if recvTimingReq indicated a stall.
        }
        PacketPtr pkt_to_retry = owner->cxlRequestRetryQueue.front();
        // Attempt to re-process this packet by calling recvTimingReq again
        // Need to pop it first to avoid potential infinite loop if recvTimingReq itself queues it back
        owner->cxlRequestRetryQueue.pop();
        DPRINTF(DecompEngine, "Retrying CXL-side request for addr %#x.\n", pkt_to_retry->getAddr());
        // recvTimingReq will handle if it needs to be re-stalled by pushing to cxlRequestRetryQueue
        // and setting cxlReqStalled = true.
        recvTimingReq(pkt_to_retry); // This might set cxlReqStalled = true
    }
    // If queue is empty, it means all retries were processed (or re-queued by recvTimingReq).
    // cxlReqStalled is primarily set by recvTimingReq if it cannot process immediately.
    // If the queue becomes empty, it doesn't automatically mean cxlReqStalled should be false.
    // It should be cleared by recvTimingReq when it can accept requests again.
    // For simplicity, if the queue is empty, we assume we are not *currently* stalled due to the queue being full.
    // However, other conditions might still keep cxlReqStalled true.
    // Let's remove the explicit setting of cxlReqStalled = false here.
    // It should be managed by recvTimingReq based on its ability to process.
}

Tick
DecompressionEngine::CXLSidePort::recvAtomic(PacketPtr pkt)
{
    // Select port based on address interleaving
    unsigned portIndex = owner->selectMemoryPort(pkt->getAddr());

    // Forward the request to the selected memory port
    return owner->memPorts[portIndex]->sendAtomic(pkt);
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
    // However, trySatisfyFunctional doesn't modify the queue structure.
    // We need to iterate over a copy or be careful if trySatisfyFunctional could trigger state changes.
    // For now, let's assume simple iteration is fine for functional check.
    // If memSendCandidateQueue can be modified by trySatisfyFunctional indirectly, this needs rework.
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

    // After checking all internal structures, forward to memory
    // Select port based on address interleaving
    unsigned portIndex = owner->selectMemoryPort(pkt->getAddr());

    assert(owner->memPorts[portIndex] != nullptr);
    owner->memPorts[portIndex]->sendFunctional(pkt);
}

AddrRangeList
DecompressionEngine::CXLSidePort::getAddrRanges() const
{
    // Combine address ranges from all memory ports
    AddrRangeList ranges;

    for (unsigned i = 0; i < owner->NUM_MEMORY_PORTS; i++) {
        if (owner->memPorts[i]) {
            AddrRangeList port_ranges = owner->memPorts[i]->getAddrRanges();
            ranges.insert(ranges.end(), port_ranges.begin(), port_ranges.end());
        }
    }

    return ranges;
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
    DPRINTF(DecompEngine, "Received request retry from memory on port %u\n", portIndex);

    // Clear stall flag for this specific port
    owner->memoryPortStalled[portIndex] = false;

    // Try processing retry queue for this specific port
    owner->tryMemPortRetry(portIndex);

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

    DecompressionRequest* chunkReq = it->second; // This is the DecompressionRequest for the chunk
    pendingRequests.erase(it);

    // All requests in pendingRequests should be chunks now
    assert(chunkReq->isChunk && "Request from pendingRequests should be a chunk.");
    DecompressionRequest* parentReq = chunkReq->parentRequest;
    assert(parentReq && "Chunk request must have a parent.");

    DPRINTF(DecompEngine, "ParentReq %p: Received response for chunk %u (addr %#x, memRespPkt %p, chunkReq->pkt %p, chunkReq %p).\n",
            parentReq, chunkReq->chunkIndex, respAddr, memRespPkt, chunkReq->pkt, chunkReq);

    if (memRespPkt->hasData() && parentReq->responseData) {
        unsigned offset = chunkReq->chunkIndex * cache_line_size;
        // Check if offset is within the parent request size
        if (offset < parentReq->pkt->getSize()) {
            // Safely calculate copy size
            size_t remainingSize = parentReq->pkt->getSize() - offset;
            size_t copySize = std::min((size_t)memRespPkt->getSize(), remainingSize);

            if (copySize > 0) {
                std::memcpy(parentReq->responseData + offset,
                           memRespPkt->getConstPtr<uint8_t>(),
                           copySize);
                DPRINTF(DecompEngine, "ParentReq %p: Copied %u bytes from chunk %u to offset %u.\n",
                        parentReq, (unsigned int)copySize, chunkReq->chunkIndex, offset);
            } else {
                // This might happen if memRespPkt->getSize() is 0 or remainingSize is 0.
                DPRINTF(DecompEngine, "ParentReq %p: Calculated copy size is 0 for chunk %u at offset %u.\n",
                     parentReq, chunkReq->chunkIndex, offset);
            }
        } else {
            warn("ParentReq %p: Chunk offset %u is out of bounds for parent request size %u (chunk %u). Skipping copy.",
                 parentReq, offset, parentReq->pkt->getSize(), chunkReq->chunkIndex);
        }
    } else {
        if (!memRespPkt->hasData()) {
            warn("ParentReq %p: Memory response packet has no data for chunk %u.",
                 parentReq, chunkReq->chunkIndex);
        }
        if (!parentReq->responseData) {
            // This should not happen if responseData is allocated in recvTimingReq
            warn("ParentReq %p: responseData is null for chunk %u. Cannot copy data.",
                 parentReq, chunkReq->chunkIndex);
        }
    }

    // Safe cleanup of chunk resources
    DPRINTF(DecompEngine, "ParentReq %p: Cleaning up chunk %u resources (memRespPkt %p, chunkReq->pkt %p, chunkReq %p).\n",
            parentReq, chunkReq->chunkIndex, memRespPkt, chunkReq->pkt, chunkReq);


    // The DecompressionRequest object for the chunk (chunkReq) owns its pkt (request to memory).
    // memRespPkt is the response packet from memory for the chunk.
    // It's possible that memRespPkt is the same packet object as chunkReq->pkt if the memory system returns it.

    bool same_packet = (memRespPkt == chunkReq->pkt);

    if (same_packet) {
        DPRINTF(DecompEngine, "ParentReq %p: memRespPkt and chunkReq->pkt are the same (%p). Will only delete once.\n",
                parentReq, memRespPkt);
        // Only delete the packet once
        delete memRespPkt; // or delete chunkReq->pkt;
        chunkReq->pkt = nullptr; // Set to nullptr to avoid double deletion by DecompressionRequest destructor or other paths
        // memRespPkt is now a dangling pointer, but we won't use it again.
    } else {
        // Normal case - different pointers
        DPRINTF(DecompEngine, "ParentReq %p: memRespPkt (%p) and chunkReq->pkt (%p) are different. Deleting both.\n",
                parentReq, memRespPkt, chunkReq->pkt);
        if (chunkReq->pkt) { // Should always exist
            delete chunkReq->pkt;
            chunkReq->pkt = nullptr;
        }
        // memRespPkt is the separate response packet from memory.
        delete memRespPkt;
    }


    // Remove chunkReq from parent's list of chunkRequests and delete the chunkReq object itself.
    // This is important to avoid dangling pointers if the parent is deleted later.
    bool found_in_parent = false;
    for (size_t i = 0; i < parentReq->chunkRequests.size(); ++i) {
        if (parentReq->chunkRequests[i] == chunkReq) {
            DPRINTF(DecompEngine, "ParentReq %p: Removing chunk %u (req %p) from parent's vector at index %u.\n",
                    parentReq, chunkReq->chunkIndex, chunkReq, (unsigned int)i);
            parentReq->chunkRequests[i] = nullptr; // Nullify in parent's list to prevent double deletion by parent's destructor
            found_in_parent = true;
            break;
        }
    }
    if (!found_in_parent) {
        DPRINTF(DecompEngine, "WARNING: ParentReq %p: Could not find chunk %u (req %p) in parent's vector for nullification.\n",
                parentReq, chunkReq->chunkIndex, chunkReq);
    }

    DPRINTF(DecompEngine, "ParentReq %p: Deleting chunk DecompressionRequest object %p.\n", parentReq, chunkReq);
    delete chunkReq; // Delete the DecompressionRequest object for the chunk

    parentReq->completedChunks++;
    DPRINTF(DecompEngine, "ParentReq %p: Completed chunk %u/%u.\n",
            parentReq, parentReq->completedChunks, parentReq->totalChunks);

    // Schedule SendReadinessUpdateEvent only if not all chunks are completed.
    // If all chunks are completed, the subsequent completeDecompression will serve as the final update.
    if (parentReq->completedChunks < parentReq->totalChunks) {
        Tick decompTime;

        if (parentReq->decompLatency_ns > 0) {
            // Convert nanoseconds to picoseconds and divide by total chunks
            decompTime = (parentReq->decompLatency_ns * 1000) / parentReq->totalChunks;
        } else {
            // Fallback to original calculation
            decompTime = (block_size * 150000) / 4096; // Scaled latency
        }

        decompTime = std::max(decompTime, Tick(1000)); // Minimum latency

        // Calculate readiness NOW at scheduling time, not when the event fires
        unsigned currentReadyCachelines = calculateReadyCachelines(parentReq);

        // Get block address for readiness update
        Addr block_addr_for_update = parentReq->pkt->getAddr() & ~(block_size - 1);

        // Create event with pre-calculated readiness value
        SendReadinessUpdateEvent* event = new SendReadinessUpdateEvent(
            this, parentReq, currentReadyCachelines, block_addr_for_update);

        schedule(event, curTick() + decompTime);

        DPRINTF(DecompEngine, "ParentReq %p: Scheduled readiness update with %u/%u cachelines ready for tick %llu (delay %llu ps, decompLatency=%.2f ns).\n",
                parentReq, currentReadyCachelines, (block_size / cache_line_size),
                curTick() + decompTime, decompTime, parentReq->decompLatency_ns);
    }


    if (parentReq->completedChunks == parentReq->totalChunks) {
        DPRINTF(DecompEngine, "ParentReq %p: All %u chunks completed for addr %#x. Original CXL Pkt: %p. Pkt cmd: %s, Pkt size: %u\n",
                parentReq, parentReq->totalChunks, parentReq->pkt->getAddr(), parentReq->pkt, parentReq->pkt->cmdString(), parentReq->pkt->getSize());

        // Data was aggregated in parentReq->responseData.
        // As per new requirement, we don't copy it to parentReq->pkt.
        // The data in parentReq->pkt will be whatever makeResponse() sets up, or original data if not a read.
        // For reads, makeResponse() should ensure it's a valid response packet.

        if (parentReq->responseData) {
            DPRINTF(DecompEngine, "ParentReq %p: responseData (%p) was populated, will be deleted. Data is not copied to parent packet %p.\n",
                    parentReq, parentReq->responseData, parentReq->pkt);
        }

        // Clean up the response data buffer
        if (parentReq->responseData) {
            delete[] parentReq->responseData;
            parentReq->responseData = nullptr;
        }

        // Make sure the command is a response and data flags are set

        if (parentReq->pkt->needsResponse()) {
            DPRINTF(DecompEngine, "ParentReq %p: Calling makeResponse() for CXL Pkt %p (current cmd: %s).\n",
                    parentReq, parentReq->pkt, parentReq->pkt->cmdString());
            parentReq->pkt->makeResponse();
            DPRINTF(DecompEngine, "ParentReq %p: CXL Pkt %p is now a %s. HasData: %d, Size: %u\n",
                    parentReq, parentReq->pkt, parentReq->pkt->cmdString(), parentReq->pkt->hasData(), parentReq->pkt->getSize());
        } else {
            DPRINTF(DecompEngine, "ParentReq %p: CXL Pkt %p (cmd: %s, size: %u) already a response or does not need makeResponse(). Skipping.\n",
                    parentReq, parentReq->pkt, parentReq->pkt->cmdString(), parentReq->pkt->getSize());
            // Ensure it is indeed a response if needsResponse() is false
            if (!parentReq->pkt->isResponse()) {
                warn("ParentReq %p: CXL Pkt %p (cmd: %s) was expected to be a response, but isNot. This might indicate an issue.",
                        parentReq, parentReq->pkt, parentReq->pkt->cmdString());
            }
        }

        // 아래 코드를 제거: 모든 청크가 완료된 후 전체 readiness 업데이트 전송하지 않음
        // unsigned totalCachelines = block_size / cache_line_size;
        // Addr block_addr_for_update = parentReq->pkt->getAddr() & ~(block_size - 1);
        // sendReadinessUpdate(block_addr_for_update, totalCachelines);

        // Queue the parent request for decompression
        decompression_queue.push(parentReq);
        tryScheduleDecompression();
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
    // Calculate decompression latency based on the provided decompression latency
    Tick decompTime;

    if (req->decompLatency_ns > 0) {
        // Convert nanoseconds to picoseconds and divide by total chunks
        decompTime = (req->decompLatency_ns * 1000) / req->totalChunks;
        DPRINTF(DecompEngine, "Using provided decompLatency %.2f ns / %u chunks = %.2f ns per chunk\n",
                req->decompLatency_ns, req->totalChunks, req->decompLatency_ns / req->totalChunks);
    } else {
        // Fallback to original calculation if no latency provided
        decompTime = (block_size * 150000) / 4096; // Scaled latency based on 4KB block
        DPRINTF(DecompEngine, "No decompLatency provided, using fallback calculation: %llu ps\n", decompTime);
    }

    decompTime = std::max(decompTime, Tick(1000)); // Minimum latency of 10ns (10000 ps)

    // Schedule decompression completion event
    DecompressionEngine::DecompressionEvent* event = new DecompressionEngine::DecompressionEvent(this, req);
    schedule(event, curTick() + decompTime); // Schedule at current time + calculated delay

    DPRINTF(DecompEngine, "Scheduled decompression for req %p (addr %#x) to complete at tick %llu (latency: %llu ps)\n",
           req, req->pkt->getAddr(), curTick() + decompTime, decompTime);
}

void
DecompressionEngine::completeDecompression(DecompressionRequest* req) // req is a parent request
{
    // This function now only handles actual decompression completion for parent requests.
    // Readiness updates are sent by sendReadinessUpdate, called by SendReadinessUpdateEvent.
    DPRINTF(DecompEngine, "Decompression complete for req %p (addr %#x, CXL Pkt %p).\n",
            req, req->pkt->getAddr(), req->pkt);
    assert(active_decompressions > 0);
    active_decompressions--;
    req->readyToRespond = true;

    assert(req->pkt->isResponse() && "Packet must be a response before sending to CXL controller");

    if (!responseStalled) {
        PacketPtr pkt_to_send = req->pkt;
        bool success = cxlPort.sendTimingResp(pkt_to_send);
        if (success) {
            DPRINTF(DecompEngine, "Successfully sent final response for req %p (using pkt %p).\n", req, pkt_to_send);
            // req->pkt is owned by CXLController. Do not delete it here.
            delete req; // Delete the DecompressionRequest wrapper.
        } else {
            responseStalled = true;
            respondingRequest = req;
            DPRINTF(DecompEngine, "CXL port stalled, delaying final response for req %p (using pkt %p).\n", req, pkt_to_send);
        }
    } else {
        completed_queue.push(req);
        DPRINTF(DecompEngine, "CXL port stalled, adding completed req %p to completed_queue.\n", req);
    }
    tryScheduleDecompression();
}

// Method to try scheduling next memory send
void
DecompressionEngine::tryScheduleNextMemSend() {
    if (memSendCandidateQueue.empty()) {
        // DPRINTF(DecompEngine, "tryScheduleNextMemSend: Queue empty.\n");
        return;
    }
    if (memSendEventScheduled) {
        // DPRINTF(DecompEngine, "tryScheduleNextMemSend: Event already scheduled.\n");
        return;
    }
    if (memoryStalled) {
        // DPRINTF(DecompEngine, "tryScheduleNextMemSend: Memory port stalled.\n");
        return;
    }

    Tick schedule_at = std::max(clockEdge(), nextMemSendAvailableAt);

    // DPRINTF(DecompEngine, "tryScheduleNextMemSend: Scheduling MemSendEvent at %llu (curTick %llu, nextAvailable %llu).\n",
    //         schedule_at, curTick(), nextMemSendAvailableAt);

    schedule(&memSendEvent, schedule_at);
    memSendEventScheduled = true;
}

// Calculate how many cachelines can be marked ready based on completed chunks
unsigned
DecompressionEngine::calculateReadyCachelines(DecompressionRequest* parentReq)
{
    // If no chunks or no progress, return 0
    if (parentReq->totalChunks == 0) { // Check totalChunks to avoid division by zero
        return 0;
    }
    // If completedChunks is 0, percentage is 0.
    if (parentReq->completedChunks == 0) {
        return 0;
    }

    // Calculate percentage of chunks completed (based on compressed data chunks)
    double completionPercentage = static_cast<double>(parentReq->completedChunks) /
                                 parentReq->totalChunks;

    // Calculate total cachelines in the original (decompressed) block
    // This should use this->block_size (or just block_size), not parentReq->pkt->getSize()
    unsigned totalCachelinesInBlock = block_size / cache_line_size;
    if (block_size % cache_line_size != 0) {
        // This should not happen if block_size is a multiple of cache_line_size
        warn("DecompressionEngine's block_size %u is not a multiple of cache_line_size %u.",
             block_size, cache_line_size);
        // Adjust if necessary, though typically block_size is a multiple of cache_line_size
        totalCachelinesInBlock = (block_size + cache_line_size -1) / cache_line_size;
    }

    // Calculate how many cachelines of the decompressed block can be marked ready
    unsigned readyCachelines = static_cast<unsigned>(completionPercentage * totalCachelinesInBlock);

    // Ensure we don't exceed total cachelines
    readyCachelines = std::min(readyCachelines, totalCachelinesInBlock);

    // DPRINTF(DecompEngine, "Block progress: %u/%u compressed chunks (%.2f%%), %u/%u decompressed cachelines ready\n",
    //         parentReq->completedChunks, parentReq->totalChunks,
    //         completionPercentage * 100.0, readyCachelines, totalCachelinesInBlock);

    return readyCachelines;
}

// Send readiness update to CXL controller
// This function is now called by SendReadinessUpdateEvent after a delay.
void
DecompressionEngine::sendReadinessUpdate(Addr block_addr_for_update, unsigned num_ready_cachelines)
{
    DPRINTF(DecompEngine, "Sending readiness update with pre-calculated value: block addr %#x, %u/%u cachelines ready.\n",
            block_addr_for_update, num_ready_cachelines, (unsigned int)(block_size / cache_line_size));

    // Create a new request for the update. Size is sizeof(unsigned) for the readiness count.
    auto update_mem_req = std::make_shared<Request>(block_addr_for_update, sizeof(unsigned), 0, 0);
    // This packet is a "message" to the CXL controller, not a real memory read from its perspective.
    // It's a ReadResp carrying data.
    PacketPtr update_pkt = new Packet(update_mem_req, MemCmd::ReadReq); // Start as ReadReq
    update_pkt->allocate();

    // Store readiness information in packet data
    *update_pkt->getPtr<unsigned>() = num_ready_cachelines;

    // CRITICAL: Make the packet a response before sending
    update_pkt->makeResponse();
    // makeResponse should set the command to ReadResp and set necessary flags.
    // Ensure that the packet is indeed a response now.
    assert(update_pkt->isResponse());
    assert(update_pkt->cmd == MemCmd::ReadResp); // Verify command after makeResponse

    // Create a decompression request wrapper for the update packet
    DecompressionRequest* update_req_obj = new DecompressionRequest(update_pkt, curTick());
    update_req_obj->isReadinessUpdate = true;
    update_req_obj->blockAddr = block_addr_for_update;
    update_req_obj->readyCachelines = num_ready_cachelines;
    update_req_obj->readyToRespond = true; // This request is ready to be sent immediately

    // Try to send it if CXL port is not stalled
    if (!responseStalled) {
        bool success = cxlPort.sendTimingResp(update_pkt); // update_pkt is now a response
        if (success) {
            DPRINTF(DecompEngine, "Successfully sent readiness update for block %#x.\n", block_addr_for_update);
            // update_pkt is now owned by the callee (CXL port/controller) or deleted by it.
            // We only need to delete the wrapper.
            delete update_req_obj;
        } else {
            responseStalled = true;
            respondingRequest = update_req_obj; // Store wrapper; its pkt is update_pkt
            DPRINTF(DecompEngine, "CXL port stalled, delaying readiness update for block %#x.\n", block_addr_for_update);
        }
    } else {
        // If CXL port is stalled, queue the readiness update.
        // The DecompressionRequest wrapper (update_req_obj) will hold the update_pkt.
        completed_queue.push(update_req_obj);
        DPRINTF(DecompEngine, "CXL port stalled, adding readiness update for block %#x to completed_queue.\n", block_addr_for_update);
    }
}

bool
DecompressionEngine::sendToMemoryPort(DecompressionRequest* req, unsigned portIndex) {
    assert(portIndex < NUM_MEMORY_PORTS);
    assert(memPorts[portIndex] != nullptr); // 포트가 초기화되었는지 확인

    MemSidePort* port = memPorts[portIndex];

    DPRINTF(DecompEngine, "Sending request for addr %#x to memory port %u (name %s)\n",
            req->pkt->getAddr(), portIndex, port->name());

    return port->sendTimingReq(req->pkt);
}
} // namespace gem5
