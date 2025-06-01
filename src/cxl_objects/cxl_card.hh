#ifndef __CXL_OBJECTS_CXL_CARD_HH__
#define __CXL_OBJECTS_CXL_CARD_HH__

#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <deque>  // Add for deque
#include <unordered_map>
#include <iostream> // Include for std::ofstream
#include <bitset> // For CachelineReadiness

#include "params/CXLController.hh"
#include "sim/sim_object.hh"
#include "debug/CXLCard.hh"
#include "base/types.hh"
#include "mem/port.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

// Forward declarations
class CXLController;
class TranslationEvent;
class BlockTracker;
class ReadinessUpdateEvent;
class UnifiedBlockOperation; // New forward declaration

// Definition for a single CXL request from the trace
struct CXLRequest
{
    bool isRead;        // true for Read, false for Write
    Addr addr;          // Memory address
    double time_us;     // Time in microseconds (floating point for precise timing)
    double comprRatio;  // Compression ratio

    // Packet pointer for pending requests
    PacketPtr pkt = nullptr;
    PacketPtr memPkt = nullptr; // For direct memory access on cache miss
    PacketPtr transPkt = nullptr; // For address translation request

    // Request tracking
    Tick arrivalTick = 0; // When the request arrived at the controller
    Tick sendTick = 0;    // When the request was sent to cache/memory
    bool sentToCache = false; // Whether this request was sent to cache
    bool cacheHit = false;    // Whether this was a cache hit

    // Translation tracking
    bool translationSent = false;  // Whether translation request was sent
    bool translationDone = false;  // Whether translation is complete
    Addr translatedAddr = 0;       // Translated address from translation

    // Block tracker reference
    BlockTracker* blockTracker = nullptr; // The block tracker this request belongs to

    // Cacheline index within block
    unsigned cachelineIndex = 0; // Index of cacheline within block

    // Completion event
    Event* completionEvent = nullptr; // Event for delayed completion

    bool completed = false; // Track completion status

    // Ready time tracking
    Tick readyTick = 0; // When this request data is ready
};

// Class for tracking cacheline readiness within a block
class CachelineReadiness {
private:
    unsigned numCachelines; // Number of cachelines in block
    std::vector<bool> ready; // Bitmap of ready cachelines

public:
    CachelineReadiness(unsigned blockSize, unsigned cachelineSize)
        : numCachelines(blockSize / cachelineSize), ready(numCachelines, false) {
        assert(blockSize % cachelineSize == 0);
    }

    // Mark cachelines as ready up to the specified count
    // 'count' is the number of cachelines that are ready (0-based from the start of the block)
    void markReadyUpTo(unsigned count) {
        for (unsigned i = 0; i < count && i < numCachelines; i++) {
            ready[i] = true;
        }
    }

    // Check if a specific cacheline is ready (index is 0-based)
    bool isReady(unsigned index) const {
        if (index >= numCachelines) return false;
        return ready[index];
    }

    // Check if all cachelines are ready
    bool allReady() const {
        for (unsigned i = 0; i < numCachelines; i++) {
            if (!ready[i]) return false;
        }
        return true;
    }

    // Get percentage of ready cachelines
    double readyPercentage() const {
        unsigned readyCount = 0;
        for (unsigned i = 0; i < numCachelines; i++) {
            if (ready[i]) readyCount++;
        }
        return static_cast<double>(readyCount) / numCachelines * 100.0;
    }
};

// 클래스 선언
class CXLController;
class RequestCompletionEvent;

// Class for tracking and processing requests to a specific memory block
class BlockTracker {
private:
    CXLController* controller;  // Reference to controller
    Addr blockAddr;             // Address of the tracked block
    std::vector<CXLRequest*> requests; // All requests to this block
    CachelineReadiness readiness;     // Tracks which cachelines are ready
    bool memoryRequestInitiated; // Whether memory request was ACTUALLY initiated
    bool cacheFillSent;         // Whether cache fill request was sent
    bool translationComplete;   // Whether address translation is complete
    Addr translatedBlockAddr;   // Translated block address
    unsigned cacheLineSize;     // Cached value of cacheline size

public:
    BlockTracker(CXLController* ctrl, Addr addr, unsigned blockSize, unsigned cacheLineSize);

    // Add a request to this block
    void addRequest(CXLRequest* req);

    // Update readiness status and process ready requests
    void updateReadiness(unsigned readyCachelines);

    // Process requests that are ready
    void processReadyRequests();

    // Check if a request is ready to complete
    bool isRequestReady(CXLRequest* req) const {
        return readiness.isReady(req->cachelineIndex);
    }

    // Complete all remaining requests (e.g., after cache fill)
    void completeAllRequests();

    // Check if any requests are not completed
    bool hasIncompleteRequests() const {
        for (CXLRequest* req : requests) {
            if (!req->completed) return true;
        }
        return false;
    }

    // Mark translation as complete and set translated address
    void setTranslationInfo(Addr transAddr) {
        translationComplete = true;
        translatedBlockAddr = transAddr;
        // Do NOT set memoryRequestInitiated here
    }

    // Mark that the memory request has been initiated
    void markMemoryRequestInitiated() {
        memoryRequestInitiated = true;
    }

    // Mark cache fill as sent
    void markCacheFillSent() {
        cacheFillSent = true;
    }

    // Getters
    Addr getBlockAddr() const { return blockAddr; }
    Addr getTranslatedBlockAddr() const { return translatedBlockAddr; }
    bool isMemoryRequestInitiated() const { return memoryRequestInitiated; } // Renamed
    bool isTranslationComplete() const { return translationComplete; }
    bool isCacheFillSent() const { return cacheFillSent; }

    // Get requests for debug
    const std::vector<CXLRequest*>& getRequests() const { return requests; }

    // Friend declarations
    friend class CXLController;
    friend class RequestCompletionEvent;
    friend class ReadinessUpdateEvent;
};

// NEW: Unified Block Operation structure
struct UnifiedBlockOperation {
    Addr blockAddr;
    std::vector<CXLRequest*> pending_cxl_requests;
    PacketPtr cachePkt = nullptr;      // Packet sent to L1 Cache
    PacketPtr transPkt = nullptr;      // Packet sent for translation

    bool cache_request_sent = false;
    bool cache_response_received = false;
    bool cache_hit_response = false;   // True if L1 cache response was a hit

    bool translation_request_sent = false;
    bool translation_response_received = false;
    Addr translated_addr = 0;

    bool completion_initiated = false; // True if completion process (due to cache hit) has started

    CXLController* controller = nullptr;

    UnifiedBlockOperation(Addr bAddr, CXLController* ctrl) : blockAddr(bAddr), controller(ctrl) {}

    ~UnifiedBlockOperation(); // Destructor to clean up packets

    void addCXLRequest(CXLRequest* cxl_req);
    void processCacheResponse(PacketPtr respPkt, bool is_hit);
    void processTranslationResponse(PacketPtr respPkt);
};

// Event for request completion with delay
class RequestCompletionEvent : public Event {
private:
    CXLController* controller;
    CXLRequest* request;

public:
    RequestCompletionEvent(CXLController* ctrl, CXLRequest* req)
        : Event(Default_Pri), controller(ctrl), request(req) {}

    void process() override;

    const char* description() const override {
        return "CXL Request Completion Event";
    }
};

// Event for readiness update processing
class ReadinessUpdateEvent : public Event {
private:
    CXLController* controller;
    Addr blockAddr;
    unsigned readyCachelines;

public:
    ReadinessUpdateEvent(CXLController* ctrl, Addr addr, unsigned ready)
        : Event(Default_Pri), controller(ctrl), blockAddr(addr), readyCachelines(ready) {}

    void process() override;

    const char* description() const override {
        return "CXL Readiness Update Event";
    }
};

// Event class for CXL requests
class CXLRequestEvent : public Event
{
  private:
    CXLRequest request;
    const std::string _name;
    CXLController *controller;

  public:
    CXLRequestEvent(CXLRequest req, const std::string &name, CXLController *ctrl)
        : request(req), _name(name), controller(ctrl) {}

    const std::string name() const override { return _name; }

    void process() override;

    // Return the CXL request
    const CXLRequest& getRequest() const { return request; }
};

class CXLController : public SimObject
{
  private:
    // File path for trace
    std::string traceFilePath;

    // Output file path for logging
    std::string outputFilePath;

    // Output file stream
    std::ofstream outputFile;

    // Fix order to match initialization
    const unsigned blockSize;
    const unsigned cacheLineSize;

    // Vector of CXL requests
    std::vector<CXLRequest> requests;

    // Request ports to the memory system
    class CXLRequestPort : public RequestPort
    {
      private:
        CXLController *controller;
        bool isCache; // Whether this is the cache port (vs mem port)

      public:
        CXLRequestPort(const std::string &name, CXLController *ctrl, bool is_cache)
            : RequestPort(name), controller(ctrl), isCache(is_cache) {}

        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

        bool isToCache() const { return isCache; }
    };

    // Port to the cache
    CXLRequestPort cachePort;

    // Port directly to memory
    CXLRequestPort memPort;

    // Port for address translation
    CXLRequestPort translationPort;

    // Queues for storing packets that need to be retried
    std::queue<PacketPtr> cacheRetryQueue;
    std::queue<PacketPtr> memRetryQueue;
    std::queue<PacketPtr> translationRetryQueue;

    // Map of block trackers by block address (used for cache misses)
    std::unordered_map<Addr, BlockTracker*> blockTrackers;

    // NEW: Map for active unified block operations
    std::unordered_map<Addr, UnifiedBlockOperation*> active_block_operations;

    // Map to track outstanding memory requests by aligned address
    std::unordered_map<Addr, CXLRequest*> outstandingMemReqs;

    // Counter for tracking sent and completed requests
    int totalRequests; // From trace file loading
    int completedRequests; // Incremented on completion

    // Variables for calculating summary statistics manually
    double totalLatencySum = 0.0;
    double hitLatencySum = 0.0;
    double missLatencySum = 0.0;
    uint64_t hitCount = 0; // Use uint64_t for potentially large counts
    uint64_t missCount = 0;

    // Load the trace file
    void loadTrace();

    // Schedule all events from the trace
    void scheduleEvents();

    // Send a memory request
    bool sendRequest(CXLRequest &req);

    // Send a request to the cache
    bool sendRequestToCache(CXLRequest &req);

    // Send a request directly to memory (for cache miss)
    bool sendRequestToMemory(CXLRequest &req, BlockTracker* tracker);

    // Send a request for address translation
    bool sendAddressTranslationRequest(CXLRequest &req);

    // Actually send the translation request to the port
    bool doSendAddressTranslationRequest(CXLRequest &req);

    // Schedule the next translation event
    void scheduleNextTranslation();

    // Check if all requests have been completed
    bool allRequestsCompleted() const;

    // Helper class to track original request when sending WriteLineReq
    class CacheCallbackState : public Packet::SenderState
    {
      public:
        BlockTracker* tracker;

        CacheCallbackState(BlockTracker* tr)
            : tracker(tr) {}
    };

    // Translation request queue and scheduling
    std::deque<CXLRequest*> translationQueue;
    TranslationEvent* translationEvent;
    Tick lastTranslationTick;

    // Flag to track if translation port is stalled
    bool translationPortStalled = false;

    // Minimum completion latency (50ns)
    const Tick minCompletionLatency;

  public:
    CXLController(const CXLControllerParams &p);
    ~CXLController();
    void startup() override;

    // Process a request
    void processRequest(const CXLRequest &reqEvent);

    // Process a cache miss
    void processCacheMiss(CXLRequest* req, PacketPtr missPkt);

    // Complete a request (called by RequestCompletionEvent)
    void completeRequestWithEvent(CXLRequest* req);

    // Try to resend packets that failed earlier (to cache or memory)
    void trySendRetries(bool toCache);

    // Try to resend translation packets that failed earlier
    void trySendTranslationRetries();

    // Dump final statistics to the output file
    void dumpStats();

    // Add processNextTranslation here so TranslationEvent can access it
    void processNextTranslation();

    // Get cacheline size
    unsigned getCachelineSize() const { return cacheLineSize; }

    // Get block size
    unsigned getBlockSize() const { return blockSize; }

    // Process a readiness update
    void processReadinessUpdate(Addr blockAddr, unsigned readyCachelines);

    // Complete a request with proper statistics and timing
    void completeRequest(CXLRequest* req, bool isHit);

    // Send cache fill request for a block
    void sendCacheFillRequest(BlockTracker* tracker);

    // Process cache fill response
    void processCacheFillResponse(Addr blockAddr);

    // Get block tracker for an address
    BlockTracker* getBlockTracker(Addr addr) {
        Addr blockAddr = addr & ~(blockSize - 1);
        auto it = blockTrackers.find(blockAddr);
        if (it != blockTrackers.end()) {
            return it->second;
        }
        return nullptr;
    }

    // Create block tracker for an address - Now primarily used during transition from UBO
    BlockTracker* createBlockTracker(Addr blockAddr, Addr translatedBlockAddr, const std::vector<CXLRequest*>& cxl_requests);

    // NEW: Transition a UnifiedBlockOperation to a BlockTracker
    void transitionToBlockTracker(UnifiedBlockOperation* ubo, Addr translated_block_addr);

    // Remove block tracker
    void removeBlockTracker(Addr addr) {
        Addr blockAddr = addr & ~(blockSize - 1);
        auto it = blockTrackers.find(blockAddr);
        if (it != blockTrackers.end()) {
            delete it->second;
            blockTrackers.erase(it);
        }
    }

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    // Friend declarations
    friend class ReadinessUpdateEvent;
    friend class RequestCompletionEvent;
    friend class BlockTracker;
    friend struct UnifiedBlockOperation; // Add friend declaration
};

} // namespace gem5

#endif // __CXL_OBJECTS_CXL_CARD_HH__
