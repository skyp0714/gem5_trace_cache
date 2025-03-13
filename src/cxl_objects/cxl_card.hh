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

namespace gem5
{

// Forward declaration
class CXLController;

// Definition for a single CXL request from the trace
struct CXLRequest
{
    bool isRead;        // true for Read, false for Write
    Addr addr;          // Memory address
    uint64_t time_us;   // Time in microseconds
    double comprRatio;  // Compression ratio
    
    // Packet pointer for pending requests
    PacketPtr pkt = nullptr;
    PacketPtr memPkt = nullptr; // For direct memory access on cache miss
    
    // Request tracking
    Tick sendTick = 0;  // When the request was sent
    bool sentToCache = false; // Whether this request was sent to cache
    bool cacheHit = false;    // Whether this was a cache hit
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
    
    // Queues for storing packets that need to be retried
    std::queue<PacketPtr> cacheRetryQueue;
    std::queue<PacketPtr> memRetryQueue;
    
    // Map to track outstanding requests
    std::unordered_map<Addr, CXLRequest*> outstandingReqs;
    
    // Counter for tracking sent and completed requests
    int totalRequests;
    int completedRequests;
    
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
    
    // Check if all requests have been completed
    bool allRequestsCompleted() const;

  public:
    CXLController(const CXLControllerParams &p);
    ~CXLController();
    void startup() override;
    
    // Called when a request is complete
    void completeRequest(PacketPtr pkt);
    
    // Process a request (called by the event)
    void processRequest(const CXLRequest &req);
    
    // Process a cache miss (send to memory directly)
    void processCacheMiss(PacketPtr pkt);
    
    // Try to resend packets that failed earlier (to cache or memory)
    void trySendRetries(bool toCache);
    
    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
};

} // namespace gem5

#endif // __CXL_OBJECTS_CXL_CARD_HH__
