#include "cxl_objects/cxl_card.hh"

#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>

#include "base/logging.hh"
#include "sim/core.hh"
#include "sim/stat_control.hh"
#include "debug/CXLCard.hh"
#include "sim/sim_exit.hh"
#include "mem/packet_access.hh"

namespace gem5
{

// Forward declaration
class TranslationEvent;

// Event to schedule translation requests
class TranslationEvent : public Event
{
  private:
    CXLController *controller;
    const std::string _name;
    friend class CXLController;

  public:
    TranslationEvent(CXLController *ctrl, const std::string &name)
        : controller(ctrl), _name(name) {}

    const std::string name() const override { return _name; }

    void process() override;
};

// BlockTracker constructor implementation
BlockTracker::BlockTracker(CXLController* ctrl, Addr addr, unsigned blockSize, unsigned cLineSize)
    : controller(ctrl),
      blockAddr(addr & ~(blockSize - 1)), // Align to block boundary
      readiness(blockSize, cLineSize),
      memoryRequestInitiated(false), // Initialize new flag
      cacheFillSent(false),
      translationComplete(false),
      translatedBlockAddr(0),
      cacheLineSize(cLineSize) {} // REMOVED: all_decompressed_data_logically_ready initialization

// BlockTracker::addRequest implementation
void
BlockTracker::addRequest(CXLRequest* req)
{
    req->blockTracker = this;
    // Calculate index using cached cacheLineSize
    req->cachelineIndex = (req->addr - blockAddr) / cacheLineSize;
    requests.push_back(req);
}

// Implementation after CXLController is defined
void
TranslationEvent::process()
{
    controller->processNextTranslation();
}

// UnifiedBlockOperation Destructor
UnifiedBlockOperation::~UnifiedBlockOperation() {
    DPRINTF(CXLCard, "Destroying UnifiedBlockOperation for block 0x%lx. cachePkt: %p, transPkt: %p\n",
            blockAddr, cachePkt, transPkt);
    // These packets are deleted here only if the UBO still "owns" them
    // (i.e., they were not successfully sent or transferred to a retry queue).
    if (cachePkt) {
        delete cachePkt;
        cachePkt = nullptr;
    }
    if (transPkt) {
        delete transPkt;
        transPkt = nullptr;
    }
}

void
UnifiedBlockOperation::addCXLRequest(CXLRequest* cxl_req) {
    // Removed completion_initiated check here to allow merging until the UBO is destroyed or transitions.
    // Cache hit path will pick up all requests in pending_cxl_requests at the moment of processing.
    // Cache miss path (transition to BT) will also take all current requests.
    pending_cxl_requests.push_back(cxl_req);
    DPRINTF(CXLCard, "UBO for 0x%lx: Added CXLReq %p (addr 0x%lx). Total pending: %lu\n",
            blockAddr, cxl_req, cxl_req->addr, pending_cxl_requests.size());
}

void
UnifiedBlockOperation::processCacheResponse(PacketPtr respPkt, bool is_hit) {
    assert(controller);
    cache_response_received = true;
    cache_hit_response = is_hit;
    DPRINTF(CXLCard, "UBO for 0x%lx: Processing cache response. Hit: %d. Current CXLReqs: %lu. RespPkt: %p\n",
            blockAddr, is_hit, pending_cxl_requests.size(), respPkt);

    // UBO no longer "owns" its cachePkt pointer if send was successful.
    // The received respPkt is now the UBO's responsibility to delete.
    delete respPkt;


    if (is_hit) {
        completion_initiated = true; // Prevent late translation from transitioning to BT
        DPRINTF(CXLCard, "UBO for 0x%lx: Cache HIT. Completing %lu CXL requests.\n",
                blockAddr, pending_cxl_requests.size());
        for (CXLRequest* cxl_req : pending_cxl_requests) {
            if (!cxl_req->completed) {
                cxl_req->cacheHit = true;
                // MODIFIED: Ensure minimum 50ns latency from arrival time
                Tick minCompletionTick = cxl_req->arrivalTick + 50000; // 50ns minimum
                Tick completionTick = std::max(curTick(), minCompletionTick);

                if (!cxl_req->completionEvent) {
                    cxl_req->completionEvent = new RequestCompletionEvent(controller, cxl_req);
                    DPRINTF(CXLCard, "UBO for 0x%lx: Scheduling HIT completion for CXLReq %p (addr 0x%lx) at %lu\n",
                            blockAddr, cxl_req, cxl_req->addr, completionTick);
                    controller->schedule(cxl_req->completionEvent, completionTick);
                } else if (!cxl_req->completionEvent->scheduled()) {
                    DPRINTF(CXLCard, "UBO for 0x%lx: Re-scheduling existing completion event for CXLReq %p (addr 0x%lx) at %lu\n",
                            blockAddr, cxl_req, cxl_req->addr, completionTick);
                    controller->schedule(cxl_req->completionEvent, completionTick);
                }
            }
        }
        // Remove UBO from active operations then delete self
        controller->active_block_operations.erase(blockAddr);
        DPRINTF(CXLCard, "UBO for 0x%lx: Cache HIT processed, UBO erased from map and will be deleted.\n", blockAddr);
        delete this;
    } else { // Cache MISS
        DPRINTF(CXLCard, "UBO for 0x%lx: Cache MISS. Pending CXLReqs: %lu.\n",
                blockAddr, pending_cxl_requests.size());

        if (!translation_request_sent) {
            // Now send the translation request since we got a cache miss
            translation_request_sent = true;

            // Create a temporary CXLRequest to use with doSendAddressTranslationRequest
            CXLRequest tempTransReq;
            tempTransReq.addr = blockAddr;
            tempTransReq.isRead = !pending_cxl_requests.empty() ?
                                  pending_cxl_requests.front()->isRead : true;

            DPRINTF(CXLCard, "UBO for 0x%lx: Sending translation request after cache miss\n", blockAddr);

            // The doSendAddressTranslationRequest function will handle timing intervals and stalled port conditions
            bool success = controller->doSendAddressTranslationRequest(tempTransReq);

            if (success) {
                DPRINTF(CXLCard, "UBO for 0x%lx: Successfully sent translation request after cache miss\n", blockAddr);
                // Packet ownership transferred to port
                tempTransReq.transPkt = nullptr;
            } else {
                DPRINTF(CXLCard, "UBO for 0x%lx: Translation request queued for later sending\n", blockAddr);
                // doSendAddressTranslationRequest already handled timing and stall conditions
            }
        }

        // Continue with existing cache miss logic
        if (translation_response_received) {
            DPRINTF(CXLCard, "UBO for 0x%lx: Translation already received. Transitioning to BlockTracker.\n", blockAddr);
            // UBO will be deleted within transitionToBlockTracker
            controller->transitionToBlockTracker(this, translated_addr);
        } else {
            DPRINTF(CXLCard, "UBO for 0x%lx: Waiting for translation response before transitioning to BlockTracker.\n", blockAddr);
        }
    }
}

void
UnifiedBlockOperation::processTranslationResponse(PacketPtr respPkt) {
    assert(controller);
    translation_response_received = true;
    // Actual translated_addr should be extracted from respPkt or determined by system.
    // For now, assuming it's set in ubo->translated_addr by the caller (CXLRequestPort)
    DPRINTF(CXLCard, "UBO for 0x%lx: Processing translation response. Translated addr: 0x%lx. RespPkt: %p\n",
            blockAddr, translated_addr, respPkt);

    // UBO no longer "owns" its transPkt pointer if send was successful.
    // The received respPkt is now the UBO's responsibility to delete.
    delete respPkt;


    if (completion_initiated) { // Cache hit was already processed and UBO is being/has been deleted
        DPRINTF(CXLCard, "UBO for 0x%lx: Completion (due to cache hit) already initiated/done. Ignoring translation response.\n", blockAddr);
        // If UBO is not yet deleted from map, it will be soon. If already deleted, 'this' is invalid.
        // This path assumes 'this' is valid if called, but UBO will be removed from map and deleted by cache hit path.
        return;
    }

    if (cache_response_received && !cache_hit_response) { // Cache miss response already received
        DPRINTF(CXLCard, "UBO for 0x%lx: Cache miss already processed. Transitioning to BlockTracker with translated_addr 0x%lx.\n",
                blockAddr, translated_addr);
        // UBO will be deleted within transitionToBlockTracker
        controller->transitionToBlockTracker(this, translated_addr);
    } else if (!cache_response_received) {
        DPRINTF(CXLCard, "UBO for 0x%lx: Translation received, but cache response pending.\n", blockAddr);
    } else { // cache_response_received && cache_hit_response
        // This case should be caught by completion_initiated. If somehow reached, it's an anomaly.
        DPRINTF(CXLCard, "UBO for 0x%lx: Translation received, but cache was a HIT and completion_initiated was false. This is unexpected.\n", blockAddr);
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
        Addr resp_addr = pkt->getAddr(); // This is block aligned address
        Addr blockAddrKey = resp_addr & ~(controller->blockSize - 1);

        DPRINTF(CXLCard, "CachePort: Received response for addr 0x%lx (block 0x%lx), pkt cmd %s\n",
                resp_addr, blockAddrKey, pkt->cmdString());

        // Handle cache fill response (WriteResp) - This part remains for BlockTracker path
        if (pkt->isResponse() && pkt->cmd == MemCmd::WriteResp) {
            DPRINTF(CXLCard, "CachePort: Received cache fill response for addr 0x%lx\n", resp_addr);
            CacheCallbackState* state = dynamic_cast<CacheCallbackState*>(pkt->popSenderState());
            if (state) {
                BlockTracker* tracker = state->tracker;
                if (tracker) {
                    DPRINTF(CXLCard, "CachePort: Cache fill for BlockTracker 0x%lx\n", tracker->getBlockAddr());
                    controller->processCacheFillResponse(tracker->getBlockAddr());
                } else {
                     warn("CachePort: Cache fill response with null tracker in state for addr 0x%lx", resp_addr);
                }
                delete state;
            } else {
                warn("CachePort: Cache fill response without CacheCallbackState for addr 0x%lx", resp_addr);
            }
            delete pkt;
            return true;
        }

        // Find UnifiedBlockOperation
        auto ubo_it = controller->active_block_operations.find(blockAddrKey);
        if (ubo_it != controller->active_block_operations.end()) {
            UnifiedBlockOperation* ubo = ubo_it->second;
            bool hit = pkt->isResponse() && !pkt->isError();
            DPRINTF(CXLCard, "CachePort: Found UBO for block 0x%lx. Processing cache response. Hit: %d\n",
                    blockAddrKey, hit);
            // Pass the original packet from port to UBO. UBO decides how to handle it.
            ubo->processCacheResponse(pkt, hit);
        } else {
            // This could be a response for a request handled by an old BlockTracker (if logic allows)
            // or an unexpected response.
            // For now, assume responses for active_block_operations or BlockTracker fills.
            // If it's a read response not for a UBO, it might be for a CXLRequest whose UBO was already deleted
            // (e.g. cache hit completed, but packet was still in flight).
            // Or it could be a response to a BlockTracker's direct cache interaction if any.
            // The original CXLRequest search logic is removed as UBOs are the primary handlers now.
            warn("CachePort: Received cache response for addr 0x%lx, but no matching UBO found. Pkt cmd: %s. Deleting pkt.\n",
                 resp_addr, pkt->cmdString());
            delete pkt;
        }
        return true;

    } else if (controller->translationPort.name() == name()) {
        // Handle address translation response
        Addr translation_lookup_addr = pkt->getAddr();
        // Derive original block address using the simple mapping rule.
        // This IS the address we consider "translated" according to the simple mapping.
        Addr original_block_addr = (translation_lookup_addr - 0x800000000) & ~(controller->blockSize - 1);

        DPRINTF(CXLCard, "TranslationPort: Received translation response for lookup_addr 0x%lx. Derived original_block_addr (used as translated_addr): 0x%lx. Pkt %p\n",
                translation_lookup_addr, original_block_addr, pkt);

        auto ubo_it = controller->active_block_operations.find(original_block_addr);
        if (ubo_it != controller->active_block_operations.end()) {
            UnifiedBlockOperation* ubo = ubo_it->second;
            // Set translated_addr using the simple mapping, ignore packet content for address.
            ubo->translated_addr = original_block_addr;
            DPRINTF(CXLCard, "TranslationPort: UBO 0x%lx setting translated_addr to 0x%lx (from simple mapping).\n",
                     ubo->blockAddr, ubo->translated_addr);
            // The content of respPkt (e.g., data payload) is ignored for determining translated_addr.
            ubo->processTranslationResponse(pkt); // pkt is still passed for its lifecycle management
        } else {
            BlockTracker* tracker = controller->getBlockTracker(original_block_addr);
            if (tracker && !tracker->isTranslationComplete()) {
                DPRINTF(CXLCard, "TranslationPort: Found BlockTracker 0x%lx waiting for translation. Processing response pkt %p.\n", original_block_addr, pkt);
                // Use the simple mapping for the translated address for the BlockTracker.
                Addr translated_val_for_bt = original_block_addr;

                bool rep_req_updated_and_valid_translation = false;
                if (!tracker->getRequests().empty()) {
                    CXLRequest* rep_req_in_bt = tracker->getRequests().front();
                    // Check if the response packet matches the one sent by the CXLRequest
                    if (rep_req_in_bt->transPkt == pkt) {
                        // Consider 0x0 as an invalid result from simple mapping (e.g. if original_block_addr was 0)
                        if (translated_val_for_bt != 0) {
                            rep_req_in_bt->translationDone = true;
                            rep_req_in_bt->translatedAddr = translated_val_for_bt;
                            DPRINTF(CXLCard, "TranslationPort: Matched transPkt. Updated rep_req_in_bt (addr 0x%lx) transDone=true, transAddr=0x%lx (simple mapped) for BT 0x%lx.\n",
                                    rep_req_in_bt->addr, translated_val_for_bt, original_block_addr);
                            rep_req_updated_and_valid_translation = true;
                        } else {
                            DPRINTF(CXLCard, "TranslationPort: Matched transPkt for BT 0x%lx, but simple mapped translated_val_for_bt is 0x0. Not marking CXLRequest as done.\n", original_block_addr);
                        }
                        delete rep_req_in_bt->transPkt;
                        rep_req_in_bt->transPkt = nullptr;
                    } else {
                        warn("TranslationPort: BT 0x%lx got translation response pkt %p, but it does not match rep_req's transPkt %p (rep_req addr 0x%lx). This response will be ignored for CXLRequest state.",
                             original_block_addr, pkt, rep_req_in_bt->transPkt, rep_req_in_bt->addr);
                        delete pkt;
                        pkt = nullptr;
                    }
                } else {
                    warn("TranslationPort: BlockTracker 0x%lx has no requests. Deleting pkt %p.\n", original_block_addr, pkt);
                    delete pkt;
                    pkt = nullptr;
                }

                if (rep_req_updated_and_valid_translation) {
                    tracker->setTranslationInfo(translated_val_for_bt); // Use new method
                    CXLRequest* representative_req = tracker->getRequests().front();
                    // Check if memory request can be sent now
                    if (representative_req->isRead && tracker->isTranslationComplete() && !tracker->isMemoryRequestInitiated()) {
                        DPRINTF(CXLCard, "TranslationPort: BT 0x%lx translation now complete. Triggering sendRequestToMemory.\n", original_block_addr);
                        controller->sendRequestToMemory(*representative_req, tracker);
                    }
                } else if (pkt) {
                     DPRINTF(CXLCard, "TranslationPort: BT 0x%lx. Rep_req not updated with valid translation or pkt mismatch. Simple mapped trans_val: 0x%lx. Pkt %p not deleted by rep_req logic.\n",
                        original_block_addr, translated_val_for_bt, pkt);
                }

            } else if (tracker && tracker->isTranslationComplete()) {
                DPRINTF(CXLCard, "TranslationPort: Received translation response for BT 0x%lx, but BT translation already complete. Ignoring pkt %p.\n", original_block_addr, pkt);
                delete pkt;
            } else {
                warn("TranslationPort: Received translation response for lookup_addr 0x%lx. No UBO, and no suitable BT found or BT not waiting. Deleting pkt %p.\n",
                     translation_lookup_addr, pkt);
                delete pkt;
            }
        }
        return true;

    } else { // Memory Port Path -> Response from DecompressionEngine
        Addr addr = pkt->getAddr();
        // Addr lineAddr = addr & ~(controller->blockSize - 1); // This is CXL-side block address if pkt is readiness update
                                                              // If pkt is final response, addr is translated block address

        DPRINTF(CXLCard, "Received response from DecompressionEngine (PKT PTR %p, pkt addr 0x%lx, size %u, cmd %s)\n",
                pkt, addr, pkt->getSize(), pkt->cmdString());

        // Check if this packet is an original CXLRequest's memPkt (final block confirmation)
        CXLRequest* cxl_req_associated_with_mem_pkt = nullptr;
        Addr original_request_addr_for_mem_pkt = 0; // For debug

        // The pkt->getAddr() here is the *translated* address used by DecompEngine for the block.
        Addr translated_block_addr_key = pkt->getAddr() & ~(controller->blockSize - 1);

        auto it_outstanding = controller->outstandingMemReqs.find(translated_block_addr_key);
        if (it_outstanding != controller->outstandingMemReqs.end()) {
            CXLRequest* candidate_req = it_outstanding->second;
            // Check if the packet pointer matches the memPkt of the candidate request
            if (candidate_req && candidate_req->memPkt == pkt) {
                cxl_req_associated_with_mem_pkt = candidate_req;
                original_request_addr_for_mem_pkt = candidate_req->addr;
            } else if (candidate_req) {
                 warn("CXLCard: Packet %p from DecompEngine for translated_addr 0x%lx matches outstanding entry, but memPkt pointer %p does not match CXLReq %p's memPkt %p.",
                      pkt, translated_block_addr_key, pkt, candidate_req, candidate_req->memPkt);
            }
        }


        if (cxl_req_associated_with_mem_pkt) {
            // This IS an original CXLRequest's memPkt. Treat as final block confirmation.
            DPRINTF(CXLCard, "Received final data block confirmation from DecompEngine for CXLReq %p (orig CXL addr 0x%lx), using its memPkt %p. Size: %u, Cmd: %s\n",
                    cxl_req_associated_with_mem_pkt, original_request_addr_for_mem_pkt, pkt, pkt->getSize(), pkt->cmdString());

            if (pkt->getSize() == sizeof(unsigned)) {
                warn("CXLCard: Final block confirmation packet for CXLReq %p (memPkt %p, orig CXL addr 0x%lx) has unexpected size %u. Expected block size %u. This might indicate an issue in DecompressionEngine's handling of the original packet or a lingering memory reuse issue.",
                     cxl_req_associated_with_mem_pkt, pkt, original_request_addr_for_mem_pkt, pkt->getSize(), controller->blockSize);
            }

            DPRINTF(CXLCard, "Removing CXLReq %p from outstandingMemReqs (key 0x%lx) as final confirmation received.\n",
                    cxl_req_associated_with_mem_pkt, translated_block_addr_key);
            controller->outstandingMemReqs.erase(it_outstanding);

            BlockTracker* tracker = cxl_req_associated_with_mem_pkt->blockTracker;

            // When final block is received, mark all cachelines in the tracker as ready.
            if (tracker) {
                unsigned total_cachelines_in_block = controller->getBlockSize() / controller->getCachelineSize();
                DPRINTF(CXLCard, "Final block confirmation for BT 0x%lx. Marking all %u cachelines as ready.\n",
                        tracker->getBlockAddr(), total_cachelines_in_block);
                tracker->updateReadiness(total_cachelines_in_block); // This will call processReadyRequests
            }

            // NEW: Trigger cache fill if appropriate
            if (tracker && tracker->isTranslationComplete() && !tracker->isCacheFillSent()) { // MODIFIED condition
                DPRINTF(CXLCard, "Final block 0x%lx received, BT translation complete. Sending cache fill.\n", // MODIFIED DPRINTF
                        tracker->getBlockAddr());
                controller->sendCacheFillRequest(tracker);
            } else if (tracker) {
                DPRINTF(CXLCard, "Final block 0x%lx received. BT state: transComplete=%d, cacheFillSent=%d. No cache fill sent now.\n", // MODIFIED DPRINTF
                        tracker->getBlockAddr(), tracker->isTranslationComplete(), tracker->isCacheFillSent());
            }


            DPRINTF(CXLCard, "Deleting memPkt %p for CXLReq %p after DecompEngine response.\n",
                    pkt, cxl_req_associated_with_mem_pkt);
            delete pkt;
            if (cxl_req_associated_with_mem_pkt->memPkt == pkt) {
                cxl_req_associated_with_mem_pkt->memPkt = nullptr;
            }

        } else {
            if (pkt->isResponse() && pkt->isRead() && !pkt->isError() &&
                pkt->hasData() && pkt->getSize() == sizeof(unsigned)) {

                unsigned readyCachelines = *pkt->getPtr<unsigned>();
                DPRINTF(CXLCard, "Received ReadinessUpdate from DecompEngine for block 0x%lx with %u ready cachelines (pkt %p)\n",
                        addr, readyCachelines, pkt);

                ReadinessUpdateEvent* event = new ReadinessUpdateEvent(
                    controller, addr, readyCachelines);
                controller->schedule(event, curTick());

                delete pkt;
            } else {
                warn("CXLCard: Received unclassifiable/unexpected response from DecompEngine (addr 0x%lx, PKT PTR %p, size %u, cmd %s). Not an original CXLReq memPkt and not a readiness update. Packet will be deleted.",
                     addr, pkt, pkt->getSize(), pkt->cmdString());
                delete pkt;
            }
        }
        return true;
    }

    return true;
}

void
CXLRequestEvent::process()
{
    // Forward the request to the controller for processing
    controller->processRequest(getRequest());
}

// Implementation of ReadinessUpdateEvent process method
void
ReadinessUpdateEvent::process()
{
    controller->processReadinessUpdate(blockAddr, readyCachelines);
}

// Implementation of RequestCompletionEvent process method
void
RequestCompletionEvent::process()
{
    controller->completeRequestWithEvent(request);
}

// Implementation of BlockTracker's updateReadiness method
void
BlockTracker::updateReadiness(unsigned readyCachelines)
{
    // Update readiness status
    readiness.markReadyUpTo(readyCachelines);

    // Process any newly ready requests
    processReadyRequests();

    // REMOVED: Logic for all_decompressed_data_logically_ready
    // Cache fill request is triggered by CXLController upon receiving the final data block.
}

// Implementation of BlockTracker's processReadyRequests method
void
BlockTracker::processReadyRequests()
{
    const Tick minLatency = 50000; // 50ns minimum latency
    Tick currentTick = curTick();

    for (CXLRequest* req : requests) {
        // Skip already completed requests
        if (req->completed) continue;

        // Check if this request's cacheline is ready
        if (readiness.isReady(req->cachelineIndex)) {
            // Set ready time if not already set
            if (req->readyTick == 0) {
                req->readyTick = currentTick;

                // Schedule completion event with minimum latency
                if (!req->completionEvent) {
                    req->completionEvent = new RequestCompletionEvent(
                        controller, req);

                    Tick completionTick = currentTick + minLatency;
                    DPRINTF(CXLCard, "Scheduling completion for req %p (addr 0x%lx) at tick %lu\n",
                            req, req->addr, completionTick);

                    controller->schedule(req->completionEvent, completionTick); // Use controller->schedule()
                }
            }
        }
    }
}

// Implementation of BlockTracker's completeAllRequests method
void
BlockTracker::completeAllRequests()
{
    const Tick minLatency = 50000; // 50ns minimum latency
    Tick currentTick = curTick();

    for (CXLRequest* req : requests) {
        // Skip already completed requests
        if (req->completed) continue;

        // Set ready time if not already set
        if (req->readyTick == 0) {
            req->readyTick = currentTick;
        }

        // Schedule completion event with minimum latency if not already scheduled
        if (!req->completionEvent) {
            req->completionEvent = new RequestCompletionEvent(
                controller, req);

            Tick completionTick = currentTick + minLatency;
            DPRINTF(CXLCard, "Scheduling completion for all remaining reqs, req %p (addr 0x%lx) at tick %lu\n",
                    req, req->addr, completionTick);

            controller->schedule(req->completionEvent, completionTick); // Use controller->schedule()
        }
    }
}

CXLController::CXLController(const CXLControllerParams &p)
    : SimObject(p),
      traceFilePath(p.trace_file),
      outputFilePath(p.output_file),
      blockSize(p.block_size),
      cacheLineSize(p.cache_line_size),
      cachePort(name() + ".cache_port", this, true),
      memPort(name() + ".mem_port", this, false),
      translationPort(name() + ".translation_port", this, false),
      totalRequests(0),
      completedRequests(0),
      translationEvent(nullptr),
      lastTranslationTick(0),
      minCompletionLatency(50000) // 50ns in ticks
{
    // Open the output file
    outputFile.open(outputFilePath);
    if (!outputFile.is_open()) {
        fatal("Could not open CXL output log file: %s", outputFilePath);
    }

    // Write the header to the output file with fixed widths
    // Add ArrivalTime(us) column (width 15)
    outputFile << std::left << std::setw(18) << "Address"
               << std::setw(15) << "ArrivalTime(us)"
               << std::setw(22) << "CompressionRatio(%)"
               << std::setw(15) << "Latency(ns)"
               << std::setw(8) << "IsHit" << std::endl;
    // Adjust separator line length
    outputFile << std::string(18 + 15 + 22 + 15 + 8, '-') << std::endl; // Separator line

    // Initialize summary stats
    totalLatencySum = 0.0;
    hitLatencySum = 0.0;
    missLatencySum = 0.0;
    hitCount = 0;
    missCount = 0;

    // Register dumpStats to be called when simulation exits
    registerExitCallback([this](){ dumpStats(); });
    DPRINTF(CXLCard, "Registered dumpStats exit callback.\n");

    // Create translation event
    translationEvent = new TranslationEvent(this, name() + ".translation_event");
}


CXLController::~CXLController()
{
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

    // Clean up any outstanding memory requests
    for (auto& pair : outstandingMemReqs) {
        CXLRequest* req = pair.second;
        if (req && !req->completed) {
            if (req->pkt) {
                delete req->pkt;
                req->pkt = nullptr;
            }
            if (req->memPkt) {
                delete req->memPkt;
                req->memPkt = nullptr;
            }
            if (req->transPkt) {
                delete req->transPkt;
                req->transPkt = nullptr;
            }
        }
    }
    outstandingMemReqs.clear();

    // Clean up translation event
    if (translationEvent) {
        delete translationEvent;
    }

    // Clean up block trackers
    for (auto& pair : blockTrackers) {
        delete pair.second;
    }
    blockTrackers.clear();

    // NEW: Clean up active_block_operations
    for (auto& pair : active_block_operations) {
        delete pair.second;
    }
    active_block_operations.clear();
}

void
CXLController::dumpStats()
{
    DPRINTF(CXLCard, "dumpStats() called.\n");
    // Calculate average latencies
    double avgTotalLatency = (completedRequests > 0) ? (totalLatencySum / completedRequests) : 0.0;
    double avgHitLatency = (hitCount > 0) ? (hitLatencySum / hitCount) : 0.0;
    double avgMissLatency = (missCount > 0) ? (missLatencySum / missCount) : 0.0;

    // Calculate hit rate
    double hitRate = (completedRequests > 0) ? ((double)hitCount / completedRequests * 100.0) : 0.0;

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
        outputFile << std::left << std::setw(25) << "Cache Hit Rate:"
                   << std::setw(15) << hitRate << " %" << std::endl;

        // Ensure data is flushed to the file
        outputFile.flush();
        DPRINTF(CXLCard, "Summary statistics written to %s.\n", outputFilePath);
    } else {
        warn("Output file stream was not open when dumpStats() was called.");
    }
}

// Process next translation
void
CXLController::processNextTranslation()
{
    if (translationQueue.empty()) {
        DPRINTF(CXLCard, "No pending translations in queue.\n");
        return;
    }

    CXLRequest* req = translationQueue.front();
    translationQueue.pop_front();

    DPRINTF(CXLCard, "Processing next translation for req %p addr 0x%lx\n",
            req, req->addr);

    // If request already completed, skip translation
    if (req->completed) {
        DPRINTF(CXLCard, "Request %p already completed, skipping translation\n", req);
        // Schedule next translation if queue not empty
        if (!translationQueue.empty()) {
            scheduleNextTranslation();
        }
        return;
    }

    // Send the translation request
    bool success = doSendAddressTranslationRequest(*req);

    // If failed to send, requeue at front
    if (!success) {
        DPRINTF(CXLCard, "Failed to send translation for req %p, requeueing\n", req);
        translationQueue.push_front(req);
    } else if (req->blockTracker) { // If successfully sent AND req is part of a block tracker
        // This CXLRequest 'req' is the one for which a translation request was just SENT.
        // 'req->translationDone' would typically be false here, unless it was already true
        // from a previous cycle and this is a re-send attempt of an already translated req.
        if (req->translationDone) { // Check if translation was already done for this req
            BlockTracker* tracker = req->blockTracker;
            Addr translated_block_addr = req->translatedAddr & ~(blockSize - 1); // req->translatedAddr should be valid

            DPRINTF(CXLCard, "processNextTranslation: CXLReq %p (for BT 0x%lx) had translationDone=true. Translated addr: 0x%lx.\n",
                    req, tracker->getBlockAddr(), translated_block_addr);

            if (translated_block_addr != 0) {
                tracker->setTranslationInfo(translated_block_addr); // Corrected method name
                DPRINTF(CXLCard, "processNextTranslation: Updated BT 0x%lx with translated_addr 0x%lx from CXLReq's existing data.\n",
                        tracker->getBlockAddr(), translated_block_addr);

                // If this update makes the BT ready for memory op, and it's a read, send it.
                if (req->isRead && tracker->isTranslationComplete() && !tracker->isMemoryRequestInitiated()) {
                    DPRINTF(CXLCard, "processNextTranslation: Triggering sendRequestToMemory for BT 0x%lx (via CXLReq %p) due to existing translation data.\n",
                            tracker->getBlockAddr(), req);
                    sendRequestToMemory(*req, tracker);
                }
            } else {
                warn("processNextTranslation: CXLReq %p (BT 0x%lx) has translationDone=true but translatedAddr is 0. BT not updated with this translation.",
                     req, tracker->getBlockAddr());
            }
        }
        // If req->translationDone is false here, the translation response is still pending.
        // CXLRequestPort::recvTimingResp will handle it when the response arrives for the BlockTracker.
    }

    // Schedule next translation if queue not empty
    if (!translationQueue.empty()) {
        scheduleNextTranslation();
    }
}

// Schedule next translation
void
CXLController::scheduleNextTranslation()
{
    // Schedule next translation after delay
    Tick nextTick = curTick() + 5000; // 5000 ticks delay

    DPRINTF(CXLCard, "Scheduling next translation at tick %lu (current: %lu)\n",
            nextTick, curTick());

    // Schedule the event
    if (!translationEvent->scheduled()) {
        schedule(translationEvent, nextTick);
    }
}

// Queue translation request
bool
CXLController::sendAddressTranslationRequest(CXLRequest &req)
{
    // Add request to queue
    DPRINTF(CXLCard, "Queueing translation request for addr 0x%lx (req %p)\n",
            req.addr, &req);

    // Find the tracked request pointer
    CXLRequest* trackedReq = nullptr;
    for (size_t i = 0; i < requests.size(); i++) { // Using size_t for vector index is standard
        if (requests[i].addr == req.addr &&
            requests[i].time_us == req.time_us &&
            !requests[i].completed)
        {
            trackedReq = &requests[i];
            break;
        }
    }

    if (!trackedReq) {
        warn("Cannot find tracked request for addr 0x%lx", req.addr);
        return false;
    }

    // Add to queue
    translationQueue.push_back(trackedReq);

    // If this is the first request or we've waited enough time
    if (translationQueue.size() == 1 ||
        (curTick() - lastTranslationTick) >= 5000)
    {
        scheduleNextTranslation();
    }

    return true;
}

// Send the translation request
bool
CXLController::doSendAddressTranslationRequest(CXLRequest &req)
{
    // Check if enough time has passed since the last translation request
    if ((curTick() - lastTranslationTick) < 5000) {
        // Not enough time has passed, we should delay this request
        DPRINTF(CXLCard, "Translation request for addr 0x%lx delayed due to timing interval (last: %lu, current: %lu)\n",
                req.addr, lastTranslationTick, curTick());

        // Create request and packet - explicitly use 8 bytes (64 bits) for translation lookup
        Addr blockAddr = req.addr & ~(blockSize - 1);
        Addr translationAddr = 0x800000000 + blockAddr;
        auto transReq = std::make_shared<Request>(translationAddr, 8, 0, 0);
        PacketPtr pkt = new Packet(transReq, MemCmd::ReadReq);
        pkt->allocate();

        // Store in request
        if (req.transPkt) {
            warn("Req %p already has a transPkt %p assigned when creating new transPkt %p", &req, req.transPkt, pkt);
            delete req.transPkt;
        }
        req.transPkt = pkt;
        req.translationSent = true;

        // Add to retry queue to be sent later
        translationRetryQueue.push(pkt);

        // Schedule the translation event if not already scheduled
        Tick nextTick = lastTranslationTick + 5000;
        if (!translationEvent->scheduled()) {
            DPRINTF(CXLCard, "Scheduling translation event at %lu due to timing interval\n", nextTick);
            schedule(translationEvent, nextTick);
        }

        return false;
    }

    // Get the original address
    Addr origAddr = req.addr;

    // Get the block address (aligned to cache line size)
    Addr blockAddr = origAddr & ~(blockSize - 1);

    // Translation table is in the second half of memory (0x800000000 - 0x80000000)
    // Calculate a lookup address in the translation table based on the block address
    Addr translationAddr = 0x800000000 + blockAddr;

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

    // If translation port is stalled, add to retry queue directly without sending
    if (translationPortStalled) {
        DPRINTF(CXLCard, "Translation port stalled, adding transPkt %p to retry queue for Req %p without sending\n", pkt, &req);
        translationRetryQueue.push(pkt);
        return false;
    }

    // Update the last translation time
    lastTranslationTick = curTick();

    // Send request
    bool success = translationPort.sendTimingReq(pkt);
    if (!success) {
        DPRINTF(CXLCard, "Translation port busy, adding transPkt %p to retry queue for Req %p\n", pkt, &req);
        translationRetryQueue.push(pkt);
        // Mark translation port as stalled
        translationPortStalled = true;
        return false;
    }

    return success;
}

// Handle translation retries
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

        // Ensure we respect the tick interval between translation requests
        if ((curTick() - lastTranslationTick) < 5000) {
            // Schedule next attempt after appropriate delay
            Tick nextTick = lastTranslationTick + 5000;
            DPRINTF(CXLCard, "Delaying translation retry for packet for addr 0x%lx until tick %lu (current: %lu)\n",
                   pkt->getAddr(), nextTick, curTick());
            if (!translationEvent->scheduled()) {
                schedule(translationEvent, nextTick);
            }
            return;
        }

        DPRINTF(CXLCard, "Attempting to retry translation packet for addr 0x%lx\n",
               pkt->getAddr());

        // Update last translation time
        lastTranslationTick = curTick();

        if (!translationPort.sendTimingReq(pkt)) {
            // Still blocked, mark as stalled and will retry later
            DPRINTF(CXLCard, "Retry sending translation packet for addr 0x%lx still blocked\n",
                   pkt->getAddr());
            translationPortStalled = true;
            return;
        }

        DPRINTF(CXLCard, "Successfully resent translation packet for addr 0x%lx\n",
               pkt->getAddr());
        translationRetryQueue.pop();
    }
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
        // Clear stalled flag when we get a retry from the translation port
        controller->translationPortStalled = false;
        controller->trySendTranslationRetries(); // Retry translation queue
    } else {
        panic("Unknown port received retry: %s", name());
    }
}

void
CXLController::trySendRetries(bool toCache)
{
    // This function only handles cacheRetryQueue and memRetryQueue
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

// Complete a request with proper statistics and timing
void
CXLController::completeRequest(CXLRequest* req, bool isHit)
{
    // Skip if already completed
    if (req->completed) {
        DPRINTF(CXLCard, "Request %p for addr 0x%lx already completed, skipping.\n", req, req->addr);
        return;
    }

    // Mark as completed
    req->completed = true;

    // Calculate latency
    Tick latency_ticks = 0;
    if (req->arrivalTick > 0) {
        latency_ticks = curTick() - req->arrivalTick;
    } else {
        warn("Request %p completed with arrivalTick=0, latency calculation might be inaccurate.", req);
        if (req->sendTick > 0) {
            latency_ticks = curTick() - req->sendTick;
        } else {
            latency_ticks = 0;
        }
    }

    double latency_ns = static_cast<double>(latency_ticks) / 1000.0;

    // Log to file
    if (outputFile.is_open()) {
        outputFile << std::left << "0x" << std::hex << std::setw(16) << req->addr << std::dec
                   << std::fixed << std::setprecision(3) << std::setw(15) << req->time_us
                   << std::fixed << std::setprecision(2) << std::setw(22) << req->comprRatio
                   << std::setw(15) << latency_ns
                   << std::setw(8) << (isHit ? "1" : "0")
                   << std::endl;
    }

    // Update statistics
    totalLatencySum += latency_ns;
    if (isHit) {
        hitLatencySum += latency_ns;
        hitCount++;
    } else {
        missLatencySum += latency_ns;
        missCount++;
    }

    DPRINTF(CXLCard, "Completed CXL Request: %s Address: 0x%lx Time: %.3f us Ratio: %.2f Latency: %.2f ns (%s) (Req: %p)\n",
           req->isRead ? "Read" : "Write",
           req->addr,
           req->time_us,
           req->comprRatio,
           latency_ns,
           isHit ? "cache hit" : "cache miss",
           req);

    // Clean up packets if needed
    // req->pkt (cache port packet for the specific CXLRequest) might be null if handled by UBO.
    // UBO's cachePkt is cleaned by UBO.
    // If req->pkt was set to UBO's cachePkt, it's a dangling pointer after UBO deletion.
    // This requires careful thought. For now, assume if req->pkt is non-null, it's specific to this CXLReq
    // and not the shared UBO packet. Or, UBO nullifies CXLRequest::pkt for its members upon its own packet deletion.
    // Let's assume CXLRequest::pkt is distinct or managed.
    if (req->pkt) {
        DPRINTF(CXLCard, "Deleting CXLRequest's cachePkt %p for req %p (addr 0x%lx)\n", req->pkt, req, req->addr);
        delete req->pkt;
        req->pkt = nullptr;
    }

    // memPkt is deleted when DecompressionEngine returns it.

    // transPkt for the specific CXLRequest (if it had its own, not UBO's)
    if (req->transPkt) {
        DPRINTF(CXLCard, "Deleting CXLRequest's transPkt %p for req %p (addr 0x%lx)\n", req->transPkt, req, req->addr);
        delete req->transPkt;
        req->transPkt = nullptr;
    }

    // Increment completed requests counter
    completedRequests++;

    // Check if all requests are completed
    if (allRequestsCompleted()) {
        DPRINTF(CXLCard, "All %d requests completed. Exiting simulation.\n", totalRequests);
        exitSimLoop("All CXL requests completed", 0);
    }
}

// Complete a request via RequestCompletionEvent
void
CXLController::completeRequestWithEvent(CXLRequest* req)
{
    // Clear event pointer
    req->completionEvent = nullptr;

    // Complete the request with appropriate hit/miss status
    completeRequest(req, req->cacheHit);
}

// Process a request
void
CXLController::processRequest(const CXLRequest &reqEvent)
{
    CXLRequest* trackedReq = nullptr;
    for (size_t i = 0; i < requests.size(); i++) {
        if (requests[i].addr == reqEvent.addr &&
            requests[i].time_us == reqEvent.time_us &&
            !requests[i].completed)
        {
            trackedReq = &requests[i];
            break;
        }
    }

    if (trackedReq == nullptr) {
        warn("Could not find matching non-completed request in request vector for addr 0x%lx time %.3f, skipping",
             reqEvent.addr, reqEvent.time_us);
        return;
    }

    if (trackedReq->arrivalTick == 0) {
        trackedReq->arrivalTick = curTick();
    }

    DPRINTF(CXLCard, "Processing CXLReq %p: Addr 0x%lx Time %.3f\n",
            trackedReq, trackedReq->addr, trackedReq->time_us);

    Addr blockAddr = trackedReq->addr & ~(blockSize - 1);

    // 1. Search for BlockTracker
    BlockTracker* tracker = getBlockTracker(blockAddr);
    if (tracker) {
        DPRINTF(CXLCard, "Found existing BlockTracker for block 0x%lx. Adding CXLReq %p.\n", blockAddr, trackedReq);
        trackedReq->cacheHit = true; // Hit on an active block operation (already past cache miss)
        tracker->addRequest(trackedReq);
        if (tracker->isRequestReady(trackedReq)) {
            Tick completionTick = curTick() + minCompletionLatency; // MODIFIED: Restore minCompletionLatency
            if (!trackedReq->completionEvent) {
                trackedReq->completionEvent = new RequestCompletionEvent(this, trackedReq);
                schedule(trackedReq->completionEvent, completionTick);
            } else if (!trackedReq->completionEvent->scheduled()) {
                schedule(trackedReq->completionEvent, completionTick);
            }
        }
    } else {
        // 2. Search for UnifiedBlockOperation
        auto ubo_it = active_block_operations.find(blockAddr);
        if (ubo_it != active_block_operations.end()) {
            UnifiedBlockOperation* ubo = ubo_it->second;
            DPRINTF(CXLCard, "Found existing UBO for block 0x%lx. Adding CXLReq %p.\n", blockAddr, trackedReq);
            ubo->addCXLRequest(trackedReq);
        } else {
            // 3. Create new UnifiedBlockOperation
            DPRINTF(CXLCard, "No BlockTracker or UBO for block 0x%lx. Creating new UBO for CXLReq %p.\n", blockAddr, trackedReq);
            UnifiedBlockOperation* new_ubo = new UnifiedBlockOperation(blockAddr, this);
            active_block_operations[blockAddr] = new_ubo;
            new_ubo->addCXLRequest(trackedReq);

            // Send L1 cache request
            auto cache_mem_req = std::make_shared<Request>(blockAddr, blockSize, 0, 0); // Request full block
            PacketPtr cache_pkt_for_ubo = new Packet(cache_mem_req, trackedReq->isRead ? MemCmd::ReadReq : MemCmd::WriteReq);
            cache_pkt_for_ubo->allocate();
            if (!trackedReq->isRead) { std::memset(cache_pkt_for_ubo->getPtr<uint8_t>(), 0xA5, blockSize); }

            new_ubo->cache_request_sent = true;
            DPRINTF(CXLCard, "UBO for 0x%lx: Sending cache request (pkt %p).\n", blockAddr, cache_pkt_for_ubo);
            if (!cachePort.sendTimingReq(cache_pkt_for_ubo)) {
                DPRINTF(CXLCard, "UBO for 0x%lx: Cache port busy for pkt %p. Adding to retry queue.\n", blockAddr, cache_pkt_for_ubo);
                cacheRetryQueue.push(cache_pkt_for_ubo);
            }

            // Translation request will be sent only after cache miss
            // No longer initiating translation request here
        }
    }
}

void
CXLController::transitionToBlockTracker(UnifiedBlockOperation* ubo, Addr translated_block_addr) {
    DPRINTF(CXLCard, "Transitioning UBO for block 0x%lx to BlockTracker. Translated addr: 0x%lx. CXLReqs: %lu\n",
            ubo->blockAddr, translated_block_addr, ubo->pending_cxl_requests.size());

    if (ubo->pending_cxl_requests.empty()) {
        warn("UBO for 0x%lx: Transitioning to BlockTracker with no CXL requests. Removing UBO.", ubo->blockAddr);
        // Ensure UBO is removed from map before deleting
        auto it = active_block_operations.find(ubo->blockAddr);
        if (it != active_block_operations.end() && it->second == ubo) {
            active_block_operations.erase(it);
        }
        delete ubo;
        return;
    }

    BlockTracker* tracker = createBlockTracker(ubo->blockAddr, translated_block_addr, ubo->pending_cxl_requests);

    bool first_req_in_batch = true;
    for (CXLRequest* cxl_req : ubo->pending_cxl_requests) {
        if (first_req_in_batch) {
            cxl_req->cacheHit = false;
            first_req_in_batch = false;
        } else {
            cxl_req->cacheHit = true;
        }
    }

    active_block_operations.erase(ubo->blockAddr);
    DPRINTF(CXLCard, "UBO for 0x%lx: Erased from active_block_operations map, BT created. Deleting UBO.\n", ubo->blockAddr);
    delete ubo;

    if (tracker && !tracker->getRequests().empty()) {
        CXLRequest* representative_req = tracker->getRequests().front();
        if (representative_req->isRead) {
            // Check if translation is complete for the tracker.
            // createBlockTracker sets it if translated_block_addr is non-zero.
            if (tracker->isTranslationComplete() && !tracker->isMemoryRequestInitiated()) { // Use new method
                 DPRINTF(CXLCard, "UBO->BT transition for 0x%lx: Translation complete for BT (transAddr 0x%lx), sending to memory.\n",
                    tracker->getBlockAddr(), tracker->getTranslatedBlockAddr());
                 // Ensure the representative CXLRequest also reflects this translation.
                 // This is crucial if sendRequestToMemory relies on CXLRequest's state.
                 if (!representative_req->translationDone || representative_req->translatedAddr != tracker->getTranslatedBlockAddr()) {
                    representative_req->translationDone = true;
                    representative_req->translatedAddr = tracker->getTranslatedBlockAddr();
                     DPRINTF(CXLCard, "Updated representative_req 0x%lx to match BT's translated addr 0x%lx.\n",
                        representative_req->addr, representative_req->translatedAddr);
                 }
                 sendRequestToMemory(*representative_req, tracker);
            } else if (!tracker->isTranslationComplete()) {
                 // This case implies translated_block_addr from UBO was 0 (or invalid)
                 DPRINTF(CXLCard, "UBO->BT transition for 0x%lx: BT's translation not complete (UBO trans_addr was 0x%lx). Re-initiating translation for BT.\n",
                    tracker->getBlockAddr(), translated_block_addr);
                // Reset CXLRequest's translation state for a new attempt
                representative_req->translationSent = false;
                representative_req->translationDone = false;
                // representative_req->transPkt should be null or will be replaced by doSendAddressTranslationRequest
                if(representative_req->transPkt) {
                    // This packet was from UBO's attempt, UBO should have cleaned it or it was transferred to retry.
                    // If it's still here, it's likely an orphaned pointer if UBO was naive.
                    // For safety, if we are re-initiating, ensure old one is not reused.
                    // However, CXLRequest::transPkt is set by doSendAddressTranslationRequest.
                    // If UBO owned its transPkt, CXLRequest::transPkt might not have been set for UBO's translation.
                    // Let's assume sendAddressTranslationRequest handles new packet creation.
                }
                sendAddressTranslationRequest(*representative_req);
            }
        } else { // Write miss
            DPRINTF(CXLCard, "UBO->BT transition for 0x%lx: Write miss. Completing CXLReqs in BT (no-write-allocate).\n", tracker->getBlockAddr());
            for (CXLRequest* cxl_req : tracker->getRequests()) {
                if (!cxl_req->completed) {
                     completeRequest(cxl_req, cxl_req->cacheHit);
                }
            }
        }
    }
}

BlockTracker*
CXLController::createBlockTracker(Addr blockAddr, Addr translatedBlockAddr, const std::vector<CXLRequest*>& cxl_requests) {
    DPRINTF(CXLCard, "Creating BlockTracker for block 0x%lx (translated 0x%lx) with %lu CXL requests.\n",
            blockAddr, translatedBlockAddr, cxl_requests.size());
    auto it = blockTrackers.find(blockAddr);
    if (it != blockTrackers.end()) {
        warn("BlockTracker for 0x%lx already exists when trying to create. Returning existing.", blockAddr);
        // Add new requests to existing tracker if necessary
        BlockTracker* existing_tracker = it->second;
        for (CXLRequest* req : cxl_requests) {
            bool already_exists = false;
            for (CXLRequest* existing_req : existing_tracker->getRequests()) {
                if (existing_req == req) {
                    already_exists = true;
                    break;
                }
            }
            if (!already_exists) existing_tracker->addRequest(req);
        }
        // If translatedBlockAddr is different and valid, update existing tracker? This is complex.
        // For now, just return existing.
        return existing_tracker;
    }

    BlockTracker* tracker = new BlockTracker(this, blockAddr, blockSize, cacheLineSize);
    blockTrackers[blockAddr] = tracker;

    // Only set translation info if translatedBlockAddr is non-zero.
    if (translatedBlockAddr != 0) {
        tracker->setTranslationInfo(translatedBlockAddr); // Use new method
    }

    for (CXLRequest* req : cxl_requests) {
        tracker->addRequest(req);
    }
    return tracker;
}

// Send request to cache - now primarily used by UBO
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

    // Find the tracked request
    CXLRequest* trackedReq = nullptr;
    for (size_t i = 0; i < requests.size(); ++i) { // Using size_t for vector index
        if (requests[i].addr == req.addr && requests[i].time_us == req.time_us && !requests[i].completed) {
            trackedReq = &requests[i];
            break;
        }
    }

    if (!trackedReq) {
        warn("Could not find original request in vector for addr 0x%lx time %.3f", req.addr, req.time_us);
        trackedReq = &req;
    }

    DPRINTF(CXLCard, "Sending CXL Request to Cache: %s Address: 0x%lx Time: %.3f us Compression Ratio: %.2f (Req: %p)\n",
           req.isRead ? "Read" : "Write",
           req.addr,
           req.time_us,
           req.comprRatio,
           trackedReq);

    // Send the packet to cache
    bool success = cachePort.sendTimingReq(pkt);
    if (!success) {
        // If sending fails immediately, add to retry queue
        DPRINTF(CXLCard, "Cache port busy, adding pkt %p to retry queue for Req %p\n", pkt, trackedReq);
        cacheRetryQueue.push(pkt);
        return true;
    }
    return success;
}

// Send request to memory - now primarily used by BlockTracker
bool
CXLController::sendRequestToMemory(CXLRequest &req, BlockTracker* tracker)
{
    assert(req.translationDone && "Translation must be complete before sending to memory");
    assert(tracker && "BlockTracker must be provided to sendRequestToMemory");

    Addr lineAddr = req.translatedAddr & ~(blockSize - 1);
    DPRINTF(CXLCard, "Using translated address 0x%lx for memory request (Req %p, BT 0x%lx)\n", lineAddr, &req, tracker->getBlockAddr());

    // Only send memory request if not already initiated for this block
    if (tracker->isMemoryRequestInitiated()) { // Use new method
        DPRINTF(CXLCard, "Memory request already initiated for block 0x%lx (BT 0x%lx), skipping\n", lineAddr, tracker->getBlockAddr());
        return true;
    }

    // Mark tracker as memory request initiated
    tracker->markMemoryRequestInitiated(); // Use new method

    // Calculate compressed size based on compression ratio
    unsigned raw_compressed_size = std::max(static_cast<unsigned>(
        static_cast<double>(blockSize) * req.comprRatio / 100.0), 1u);

    // Round up to nearest cacheline size
    unsigned compressedSize = ((raw_compressed_size + cacheLineSize - 1) / cacheLineSize) * cacheLineSize;

    DPRINTF(CXLCard, "Original size: %u bytes, Raw compressed size: %u bytes, Final rounded compressed size: %u bytes (Ratio: %.2f%%, CacheLineSize: %u)\n",
            blockSize, raw_compressed_size, compressedSize, req.comprRatio, cacheLineSize);

    // Create the request for memory with the compressed size
    auto memReq = std::make_shared<Request>(lineAddr, compressedSize, 0, 0);

    // Create the packet for direct memory access
    PacketPtr pkt = new Packet(memReq, req.isRead ? MemCmd::ReadReq : MemCmd::WriteReq);

    // Allocate memory
    pkt->allocate();

    // Fill with data for write
    if (!req.isRead) {
        std::memset(pkt->getPtr<uint8_t>(), 0xA5, compressedSize);
    }

    // Store compression ratio in packet data
    if (pkt->hasData() && pkt->getSize() >= sizeof(double)) {
        *reinterpret_cast<double*>(pkt->getPtr<uint8_t>()) = req.comprRatio;
    }

    // Store packet in request
    if (req.memPkt) {
        warn("Req %p already has a memPkt %p assigned when creating new memPkt %p", &req, req.memPkt, pkt);
        delete req.memPkt;
    }
    req.memPkt = pkt;

    DPRINTF(CXLCard, "Sending CXL Request to Memory: %s Address: 0x%lx (orig: 0x%lx) Size: %u bytes, Ratio: %.2f%% (Req: %p, BT: 0x%lx)\n",
           req.isRead ? "Read" : "Write", lineAddr, req.addr, compressedSize, req.comprRatio, &req, tracker->getBlockAddr());

    // Track memory request
    outstandingMemReqs[lineAddr] = &req;

    // Send to memory
    bool success = memPort.sendTimingReq(pkt);
    if (!success) {
        DPRINTF(CXLCard, "Memory port busy, adding memPkt %p to retry queue for Req %p\n", pkt, &req);
        memRetryQueue.push(pkt);
        return true;
    }

    return success;
}

// Process cache fill response
void
CXLController::processCacheFillResponse(Addr blockAddr)
{
    DPRINTF(CXLCard, "Received cache fill response for block 0x%lx\n", blockAddr);

    // Find block tracker
    BlockTracker* tracker = getBlockTracker(blockAddr);
    if (!tracker) {
        warn("Cache fill response for unknown block 0x%lx", blockAddr);
        return;
    }

    // Complete all remaining requests in tracker
    tracker->completeAllRequests();

    // Remove block tracker (after a short delay to allow requests to complete)
    // For now, we'll just schedule cleanup immediately
    DPRINTF(CXLCard, "Scheduling cleanup of block tracker for 0x%lx\n", blockAddr);

    // In a real implementation, we might want a delay here before cleanup
    removeBlockTracker(blockAddr);
}

// Process readiness updates from decompression engine
void
CXLController::processReadinessUpdate(Addr blockAddr, unsigned readyCachelines)
{
    DPRINTF(CXLCard, "Received readiness update for block 0x%lx: %u cachelines ready\n",
            blockAddr, readyCachelines);

    // Find block tracker
    BlockTracker* tracker = getBlockTracker(blockAddr);
    if (!tracker) {
        warn("Readiness update for unknown block 0x%lx", blockAddr);
        return;
    }

    // Update readiness in tracker
    tracker->updateReadiness(readyCachelines);
}

// Cache fill request
void
CXLController::sendCacheFillRequest(BlockTracker* tracker)
{
    // Mark cache fill as sent in tracker
    tracker->markCacheFillSent();

    Addr blockAddr = tracker->getBlockAddr();

    DPRINTF(CXLCard, "Sending cache fill request for block 0x%lx\n", blockAddr);

    auto fillReq = std::make_shared<Request>(blockAddr, blockSize, 0, 0);
    PacketPtr fillPkt = new Packet(fillReq, MemCmd::WriteLineReq);
    fillPkt->allocate();

    // Per user request, do not copy actual data. Fill with a pattern.
    DPRINTF(CXLCard, "Filling cache fill request for block 0x%lx with pattern.\n", blockAddr);
    std::memset(fillPkt->getPtr<uint8_t>(), 0xA5, blockSize);

    fillPkt->pushSenderState(new CacheCallbackState(tracker));

    bool success = cachePort.sendTimingReq(fillPkt);
    if (!success) {
        DPRINTF(CXLCard, "Cache fill request failed for block 0x%lx, adding to retry queue\n", blockAddr);
        cacheRetryQueue.push(fillPkt);
    } else {
        DPRINTF(CXLCard, "Successfully sent cache fill request for block 0x%lx\n", blockAddr);
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
        // Skip comment lines starting with //
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
            continue;
        }

        // Skip empty lines
        if (line.empty() || line.find_first_not_of(" \t\n\r") == std::string::npos) {
            continue;
        }

        std::istringstream iss(line);
        char rw;
        Addr addr;
        double time_us;
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

    DPRINTF(CXLCard, "Loaded %u CXL requests from trace file: %s\n",
           (unsigned int)requests.size(), traceFilePath.c_str()); // Cast to unsigned int for %u
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
