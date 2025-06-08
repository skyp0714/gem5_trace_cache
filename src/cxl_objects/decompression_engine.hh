#ifndef __CXL_DECOMPRESSION_ENGINE_HH__
#define __CXL_DECOMPRESSION_ENGINE_HH__

#include <queue>
#include <map>
#include <vector> // Added for std::vector

#include "mem/port.hh"
#include "debug/DecompEngine.hh"
#include "params/DecompressionEngine.hh"
#include "sim/clocked_object.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class DecompressionEngine; // Forward declaration

/**
 * Request structure that holds the information about the request
 * including the original packet
 */
struct DecompressionRequest {
    PacketPtr pkt;          // Original request packet from CXLController (for parent)
                            // or chunk packet to memory (for chunk)
    PacketPtr respPkt;      // Response packet from memory (for chunk)
                            // or aggregated response packet (for parent, if created separately)
                            // In the new design, parent's pkt will be reused for final response.
    Tick arrivalTime;
    bool readyToRespond;

    // For chunking
    bool isChunk;
    DecompressionRequest* parentRequest; // If this is a chunk, points to parent
    std::vector<DecompressionRequest*> chunkRequests; // If this is a parent, holds its chunks
    char* responseData;                  // If this is a parent, buffer for aggregated chunk data
    unsigned chunkIndex;                 // If this is a chunk, its index
    unsigned totalChunks;                // Total number of chunks for this request
    unsigned completedChunks;            // Number of chunks that have received responses

    // NEW: For tracking readiness updates
    bool isReadinessUpdate;         // Whether this is a readiness update packet
    Addr blockAddr;                 // Block address for readiness updates
    unsigned readyCachelines;       // Number of cachelines ready in block

    // NEW: For decompression latency
    double decompLatency_ns;        // Decompression latency in nanoseconds

    DecompressionRequest(PacketPtr _pkt, Tick _time)
        : pkt(_pkt), respPkt(nullptr), arrivalTime(_time), readyToRespond(false),
          isChunk(false), parentRequest(nullptr), responseData(nullptr),
          chunkIndex(0), totalChunks(0), completedChunks(0), // totalChunks default to 0, set explicitly for parents
          isReadinessUpdate(false), blockAddr(0), readyCachelines(0),
          decompLatency_ns(0.0) {}

    // Destructor to clean up dynamically allocated resources if any owned by this struct directly
    ~DecompressionRequest() {
        // If this is a parent request, it owns responseData and its chunkRequest objects
        if (!isChunk) {
            // Clean up responseData if it exists
            delete[] responseData;
            responseData = nullptr;

            // Safely clean up chunk requests
            for (size_t i = 0; i < chunkRequests.size(); i++) {
                DecompressionRequest* chunk_req = chunkRequests[i];
                if (chunk_req) {
                    // Only delete chunk's packet if it still exists
                    // Chunk's pkt is owned by DecompressionRequest object for the chunk
                    if (chunk_req->pkt) {
                        delete chunk_req->pkt;
                        chunk_req->pkt = nullptr;
                    }
                    // Chunk's respPkt is the memory response, deleted when handled or here if pending
                    if (chunk_req->respPkt) {
                        delete chunk_req->respPkt;
                        chunk_req->respPkt = nullptr;
                    }
                    delete chunk_req; // Delete the DecompressionRequest object for the chunk
                }
            }
            chunkRequests.clear();
        }
        // If this is a chunk, its pkt and respPkt are managed by its own lifecycle or by the parent's cleanup of chunkRequests.
        // The original pkt (from CXLController) for a parent is not deleted here.
    }

    // Prevent copying to avoid double deletion issues with raw pointers.
    DecompressionRequest(const DecompressionRequest&) = delete;
    DecompressionRequest& operator=(const DecompressionRequest&) = delete;
    // Allow move
    DecompressionRequest(DecompressionRequest&& other) noexcept
        : pkt(other.pkt), respPkt(other.respPkt), arrivalTime(other.arrivalTime),
          readyToRespond(other.readyToRespond), isChunk(other.isChunk),
          parentRequest(other.parentRequest), chunkRequests(std::move(other.chunkRequests)),
          responseData(other.responseData), chunkIndex(other.chunkIndex),
          totalChunks(other.totalChunks), completedChunks(other.completedChunks),
          isReadinessUpdate(other.isReadinessUpdate), blockAddr(other.blockAddr),
          readyCachelines(other.readyCachelines), decompLatency_ns(other.decompLatency_ns) {
        other.pkt = nullptr;
        other.respPkt = nullptr;
        other.parentRequest = nullptr;
        other.responseData = nullptr;
    }
    DecompressionRequest& operator=(DecompressionRequest&& other) noexcept {
        if (this != &other) {
            // Clean up existing resources
            if (!isChunk) {
                delete[] responseData;
                for (DecompressionRequest* chunk_req : chunkRequests) {
                    if (chunk_req) { // Check if the pointer is not null
                        delete chunk_req->pkt; // chunk_req owns its pkt
                        delete chunk_req->respPkt; // and its respPkt
                        delete chunk_req;
                    }
                }
                chunkRequests.clear();
            } else { // This is a chunk, clean its own packets
                delete pkt;
                delete respPkt;
            }


            pkt = other.pkt;
            respPkt = other.respPkt;
            arrivalTime = other.arrivalTime;
            readyToRespond = other.readyToRespond;
            isChunk = other.isChunk;
            parentRequest = other.parentRequest;
            chunkRequests = std::move(other.chunkRequests); // if other is parent
            responseData = other.responseData; // if other is parent
            chunkIndex = other.chunkIndex;
            totalChunks = other.totalChunks;
            completedChunks = other.completedChunks;
            isReadinessUpdate = other.isReadinessUpdate;
            blockAddr = other.blockAddr;
            readyCachelines = other.readyCachelines;
            decompLatency_ns = other.decompLatency_ns;

            other.pkt = nullptr;
            other.respPkt = nullptr;
            other.parentRequest = nullptr;
            other.responseData = nullptr;
        }
        return *this;
    }
};


class DecompressionEngine : public ClockedObject
{
  protected:
    class CXLSidePort : public ResponsePort
    {
      private:
        /// The decompression engine this port belongs to
        DecompressionEngine *owner;

      public:
        CXLSidePort(const std::string &_name, DecompressionEngine *_owner)
            : ResponsePort(_name), owner(_owner) {}

        bool recvTimingReq(PacketPtr pkt) override;

        void recvRespRetry() override;

        void trySendRetries(); // For CXL request retries

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
    };

    /**
     * Port on the memory side
     */
    class MemSidePort : public RequestPort
    {
      private:
        /// The decompression engine this port belongs to
        DecompressionEngine *owner;

      public:
        MemSidePort(const std::string &_name, DecompressionEngine *_owner)
            : RequestPort(_name), owner(_owner) {}

        bool recvTimingResp(PacketPtr pkt) override;

        void recvReqRetry() override;
    };

    /**
     * Handle a response from memory
     */
    void handleResponse(PacketPtr pkt);

    /**
     * Try to schedule the next available decompression task if an engine is free.
     */
    void tryScheduleDecompression();

    /**
     * Schedule decompression event for a specific request, assuming an engine is available.
     */
    void scheduleDecompression(DecompressionRequest* req);

    /**
     * Complete decompression and send response
     */
    void completeDecompression(DecompressionRequest* req);

    /**
     * Try sending requests queued due to memory port being busy.
     */
    void trySendMemoryRetries(); // Kept for conceptual clarity, actual sending is via tryScheduleNextMemSend

    // Event for sending subsequent chunks with delay
    class ChunkSendEvent : public Event {
      private:
        DecompressionEngine *engine;
        DecompressionRequest *parentRequest; // The original request being chunked
        unsigned nextChunkIndex;             // The index of the next chunk to send

      public:
        ChunkSendEvent(DecompressionEngine *_engine, DecompressionRequest *_parentReq, unsigned _nextIdx)
            : Event(Default_Pri), engine(_engine), parentRequest(_parentReq), nextChunkIndex(_nextIdx) {}

        void process() override; // Implementation in .cc

        const char *description() const override {
            return "DecompressionEngine chunk send event";
        }
    };

    // ADDED: Event for actual decompression completion
    class DecompressionEvent : public Event {
      private:
        DecompressionEngine *engine;
        DecompressionRequest *request; // The request that has finished decompression
      public:
        DecompressionEvent(DecompressionEngine *_engine, DecompressionRequest *_req)
            : Event(Default_Pri), engine(_engine), request(_req) {}

        void process() override {
            // This method will be implemented in the .cc file or inline here.
            // For consistency with other events, let's assume it calls a method on the engine.
            engine->completeDecompression(request);
        }

        const char *description() const override {
            return "DecompressionEngine decompression completion event";
        }
    };

    // ADDED: Event for scheduling next memory send
    class ScheduleMemSendEvent : public Event {
      private:
        DecompressionEngine *engine;
      public:
        ScheduleMemSendEvent(DecompressionEngine *_engine)
            : Event(Default_Pri), engine(_engine) {}
        void process() override; // Implementation in .cc
        const char *description() const override {
            return "DecompressionEngine schedule memory send event";
        }
    };

    // MODIFIED: Event for sending readiness update with pre-calculated value
    class SendReadinessUpdateEvent : public Event {
      private:
        DecompressionEngine *engine;
        DecompressionRequest *parentRequest; // The parent request for which to send update
        unsigned readyCachelines; // Pre-calculated readiness value
        Addr blockAddr; // Block address for update
      public:
        SendReadinessUpdateEvent(DecompressionEngine *_engine, DecompressionRequest *_parentReq,
                               unsigned _readyCachelines, Addr _blockAddr)
            : Event(Default_Pri), engine(_engine), parentRequest(_parentReq),
              readyCachelines(_readyCachelines), blockAddr(_blockAddr) {}

        void process() override {
            // Pass the pre-calculated readiness value to sendReadinessUpdate
            engine->sendReadinessUpdate(blockAddr, readyCachelines);
        }

        const char *description() const override {
            return "DecompressionEngine send readiness update event";
        }
    };

    /**
     * Send readiness update to CXL controller with pre-calculated readiness value
     */
    void sendReadinessUpdate(Addr blockAddr, unsigned readyCachelines);

    /**
     * Calculate how many cachelines can be made ready based on chunks completed
     */
    unsigned calculateReadyCachelines(DecompressionRequest* parentReq);

    /// CXL side port
    CXLSidePort cxlPort;

    /// Memory side port
    MemSidePort memPort;

    /// Queue for requests that received memory response and wait for decompression engine
    std::queue<DecompressionRequest*> decompression_queue; // Holds parent requests ready for decompression

    /// Queue for requests that finished decompression but are waiting for CXL port
    std::queue<DecompressionRequest*> completed_queue; // Holds parent requests or readiness updates

    /// Map of requests sent to memory, waiting for response (key: address of chunk packet)
    std::map<Addr, DecompressionRequest*> pendingRequests; // Holds chunk requests

    /// Flag for when we're stalled waiting for memory port to become available
    bool memoryStalled;

    /// Flag for when we're stalled waiting for CXL controller to accept response
    bool responseStalled;

    /// Request whose response is currently stalled waiting for CXL controller
    DecompressionRequest* respondingRequest; // Holds parent request or readiness update

    /// Compression block size in bytes
    const unsigned block_size;

    /// Cacheline size in bytes
    const unsigned cache_line_size;

    /// Number of parallel decompression engines
    const unsigned num_engines; // ADDED

    /// Number of currently active decompression operations
    unsigned active_decompressions; // ADDED

    const Tick chunkSendDelay; // Delay between creating chunks and adding them to queue

    // For CXL-side request retries
    std::queue<PacketPtr> cxlRequestRetryQueue; // Queue for CXL requests that couldn't be processed immediately
    bool cxlReqStalled;                         // Flag if CXL side is stalled for new requests

    // For parent requests whose first chunk send failed due to memory stall
    std::queue<DecompressionRequest*> memParentRetryQueue; // This might also be removable if all go via memSendCandidateQueue

    // ADDED: For memory request serialization
    std::queue<DecompressionRequest*> memSendCandidateQueue;
    Tick nextMemSendAvailableAt;
    const Tick interMemoryRequestDelay; // Changed from static const
    ScheduleMemSendEvent memSendEvent;
    bool memSendEventScheduled; // To prevent scheduling multiple send events


  public:
    DecompressionEngine(const DecompressionEngineParams &params);
    ~DecompressionEngine();

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    // New method to send the next chunk, called by ChunkSendEvent
    void sendNextChunk(DecompressionRequest* parentReq, unsigned chunkIndex);
    void tryScheduleNextMemSend();
};

} // namespace gem5

#endif // __CXL_DECOMPRESSION_ENGINE_HH__
