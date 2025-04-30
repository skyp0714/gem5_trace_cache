#ifndef __CXL_OBJECTS_CXL_CARD_HH__
#define __CXL_OBJECTS_CXL_CARD_HH__

#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>

#include "params/CXLController.hh"
#include "sim/sim_object.hh"
#include "debug/CXLCard.hh"
#include "base/types.hh"
#include "mem/port.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "base/statistics.hh"  // Add this include for stats

namespace gem5
{

// Forward declaration
class CXLController;

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
    Tick sendTick = 0;  // When the request was sent
    bool sentToCache = false; // Whether this request was sent to cache
    bool cacheHit = false;    // Whether this was a cache hit

    // Translation tracking
    bool translationSent = false;  // Whether translation request was sent
    bool translationDone = false;  // Whether translation is complete
    Addr translatedAddr = 0;       // Translated address from translation

    // Dependency tracking
    bool isWaitingForMemory = false;   // Whether this request is waiting for another memory request
    CXLRequest* waitingForRequest = nullptr;  // The request this one is waiting for

    bool completed = false; // Ensure this flag exists
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

    // Cache line size
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

    // Map to track outstanding requests (allows multiple requests per address)
    std::unordered_multimap<Addr, CXLRequest*> outstandingReqs;

    // Map to track outstanding memory requests by aligned address
    std::unordered_map<Addr, CXLRequest*> outstandingMemReqs;

    // Map to track requests dependent on another request (key: primary request ptr)
    std::unordered_map<CXLRequest*, std::vector<CXLRequest*>> dependentReqs;

    // Counter for tracking sent and completed requests
    int totalRequests;
    int completedRequests;

    // Statistics
    struct CXLStats : public statistics::Group
    {
        CXLStats(statistics::Group *parent);

        // Mean access latency for all requests
        statistics::Average meanAccessLatency;

        // Separate stats for read and write operations
        statistics::Average readLatency;
        statistics::Average writeLatency;

        // Separate stats for hits and misses
        statistics::Average hitLatency;
        statistics::Average missLatency;

        // Separate stats for reads and writes with hits and misses
        statistics::Average readHitLatency;
        statistics::Average readMissLatency;
        statistics::Average writeHitLatency;
        statistics::Average writeMissLatency;

        // Count statistics for hits and misses
        statistics::Scalar totalRequests;
        statistics::Scalar totalHits;
        statistics::Scalar totalMisses;
        statistics::Scalar readHits;
        statistics::Scalar readMisses;
        statistics::Scalar writeHits;
        statistics::Scalar writeMisses;
        statistics::Formula hitRate;
    } stats;

    // Load the trace file
    void loadTrace();

    // Schedule all events from the trace
    void scheduleEvents();

    // Send a memory request
    bool sendRequest(CXLRequest &req);

    // Send a request to the cache
    bool sendRequestToCache(CXLRequest &req);

    // Send a request directly to memory (for cache miss)
    bool sendRequestToMemory(CXLRequest &req);

    // Send a request for address translation
    bool sendAddressTranslationRequest(CXLRequest &req);

    // Check if all requests have been completed
    bool allRequestsCompleted() const;

    // Complete dependent requests waiting for the given request
    void completeDependentRequests(CXLRequest* req);

    // Helper class to track original request when sending WriteLineReq
    class CacheCallbackState : public Packet::SenderState
    {
      public:
        CXLRequest* origReq;
        PacketPtr origPkt;

        CacheCallbackState(CXLRequest* req, PacketPtr pkt)
            : origReq(req), origPkt(pkt) {}
    };

  public:
    CXLController(const CXLControllerParams &p);
    ~CXLController();
    void startup() override;

    // Called when a request is complete
    // Accepts the request pointer and an optional response packet (e.g., for cache hits)
    void completeRequest(CXLRequest* req, PacketPtr respPkt = nullptr);

    // Process a request (called by the event)
    void processRequest(const CXLRequest &req);

    // Process a cache miss (send to memory directly)
    // Accepts the request pointer and the cache miss packet
    void processCacheMiss(CXLRequest* req, PacketPtr missPkt);

    // Try to resend packets that failed earlier (to cache or memory)
    void trySendRetries(bool toCache);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
};

} // namespace gem5

#endif // __CXL_OBJECTS_CXL_CARD_HH__
