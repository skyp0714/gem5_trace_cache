#ifndef __CXL_DECOMPRESSION_ENGINE_HH__
#define __CXL_DECOMPRESSION_ENGINE_HH__

#include <queue>

#include "mem/port.hh"
#include "params/DecompressionEngine.hh"
#include "sim/clocked_object.hh"
#include "sim/sim_object.hh"

namespace gem5
{

/**
 * The DecompressionEngine is responsible for simulating the decompression
 * of data between memory and the CXL controller. It receives requests from
 * the CXL controller, forwards them to memory, and applies a decompression
 * latency to responses before sending them back.
 */
class DecompressionEngine : public ClockedObject
{
  protected:
    /**
     * Port on the CXL controller side
     */
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
     * Request structure that holds the information about the request
     * including the original packet
     */
    struct DecompressionRequest {
        PacketPtr pkt;
        Tick arrivalTime;
        bool readyToRespond;

        DecompressionRequest(PacketPtr _pkt, Tick _time)
            : pkt(_pkt), arrivalTime(_time), readyToRespond(false) {}
    };

    /**
     * Handle a response from memory
     */
    void handleResponse(PacketPtr pkt);

    /**
     * Schedule decompression to complete
     */
    void scheduleDecompression(DecompressionRequest* req);

    /**
     * Complete decompression and send response
     */
    void completeDecompression(DecompressionRequest* req);

    /**
     * Process next pending request
     */
    void processNextRequest();

    /**
     * Event scheduled when decompression completes
     */
    class DecompressionEvent : public Event
    {
      private:
        DecompressionEngine *engine;
        DecompressionRequest *req;

      public:
        DecompressionEvent(DecompressionEngine *_engine, DecompressionRequest *_req)
            : Event(Default_Pri), engine(_engine), req(_req) {}

        void process() override {
            engine->completeDecompression(req);
        }

        const char *description() const override {
            return "DecompressionEngine completion event";
        }
    };

    /// CXL side port
    CXLSidePort cxlPort;

    /// Memory side port
    MemSidePort memPort;

    /// Queues holding requests that are in progress
    std::queue<DecompressionRequest*> requestQueue;

    /// Currently pending requests (being decompressed)
    std::map<Addr, DecompressionRequest*> pendingRequests;

    /// Flag for when we're stalled waiting for memory
    bool memoryStalled;

    /// Flag for when we're stalled waiting for CXL controller to accept response
    bool responseStalled;

    /// Currently responding request
    DecompressionRequest* respondingRequest;

    /// Compression block size in bytes
    const unsigned block_size;

  public:
    DecompressionEngine(const DecompressionEngineParams &params);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
};

} // namespace gem5

#endif // __CXL_DECOMPRESSION_ENGINE_HH__
