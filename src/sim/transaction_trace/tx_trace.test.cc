#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "tx_trace/tx_text_writer.hh"
#include "tx_trace/tx_trace.hh"

using namespace tx_trace;

namespace
{

/**
 * Helper: create a TxTrace backed by a stringstream, returning both
 * the trace and a raw pointer to the stream for inspection.
 */
std::pair<std::unique_ptr<TxTrace>, std::stringstream *>
makeTrace()
{
    auto ss = std::make_unique<std::stringstream>();
    auto *ss_ptr = ss.get();
    auto writer = std::make_unique<TxTextWriter>(std::move(ss));
    auto trace = std::make_unique<TxTrace>(std::move(writer));
    return {std::move(trace), ss_ptr};
}

/** Check that a line appears in multi-line output. */
bool
containsLine(const std::string &output, const std::string &line)
{
    std::istringstream iss(output);
    std::string cur;
    while (std::getline(iss, cur)) {
        if (cur == line) {
            return true;
        }
    }
    return false;
}

/** Count how many lines contain the given substring. */
int
countMatches(const std::string &output, const std::string &substr)
{
    int count = 0;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(substr) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// ==================================================================
// Stream and generator creation
// ==================================================================

TEST(TxTraceTest, StreamAndGeneratorCreation)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid =
        trace->addStream("system.ruby.l1_cntrl0.sequencer", "Sequencer");
    uint64_t memreq_gen = trace->addGeneratorPair(sid, "memreq");
    (void)memreq_gen;

    std::string out = ss->str();

    EXPECT_TRUE(containsLine(
        out, "scv_tr_stream (ID 0, name "
             "\"system.ruby.l1_cntrl0.sequencer\", kind \"Sequencer\")"));
    EXPECT_TRUE(containsLine(
        out, "scv_tr_generator (ID 0, name \"memreq\", scv_tr_stream 0)"));
    EXPECT_TRUE(containsLine(out,
                             "scv_tr_generator (ID 1, name \"memreq.events\","
                             " scv_tr_stream 0)"));
}

// ==================================================================
// Root transaction with attributes
// ==================================================================

TEST(TxTraceTest, RootTransactionWithAttributes)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid = trace->addStream("seq0", "Sequencer");
    uint64_t gen = trace->addGeneratorPair(sid, "memreq");

    TraceId root =
        trace->createRootTransaction(gen, 1000,
                                     {
                                         {"address", uint64_t(0x80001000)},
                                         {"type", std::string("LD")},
                                         {"size", uint64_t(8)},
                                     });

    trace->retireTransaction(root, 1045,
                             {
                                 {"hit", true},
                             });

    std::string out = ss->str();

    EXPECT_TRUE(containsLine(out, "tx_begin 1 0 1000"));
    EXPECT_TRUE(containsLine(
        out, "tx_record_attribute 1 \"address\" UNSIGNED = 2147487744"));
    EXPECT_TRUE(
        containsLine(out, "tx_record_attribute 1 \"type\" STRING = \"LD\""));
    EXPECT_TRUE(
        containsLine(out, "tx_record_attribute 1 \"size\" UNSIGNED = 8"));
    EXPECT_TRUE(
        containsLine(out, "tx_record_attribute 1 \"hit\" BOOLEAN = true"));
    EXPECT_TRUE(containsLine(out, "tx_end 1 0 1045"));
}

// ==================================================================
// Child transaction with parent_of relation
// ==================================================================

TEST(TxTraceTest, ChildTransactionRelation)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid = trace->addStream("seq0", "Sequencer");
    uint64_t memreq_gen = trace->addGeneratorPair(sid, "memreq");
    uint64_t flit_gen = trace->addGeneratorPair(sid, "flit");

    TraceId root = trace->createRootTransaction(memreq_gen, 1000);
    TraceId flit =
        trace->createChildTransaction(flit_gen, root, 1010,
                                      {
                                          {"flit_index", uint64_t(0)},
                                          {"vnet", uint64_t(2)},
                                      });

    trace->retireTransaction(flit, 1040);
    trace->retireTransaction(root, 1045);

    std::string out = ss->str();

    // Root transaction
    EXPECT_TRUE(containsLine(out, "tx_begin 1 0 1000"));
    EXPECT_TRUE(containsLine(out, "tx_end 1 0 1045"));

    // Flit child transaction
    EXPECT_TRUE(containsLine(out, "tx_begin 2 2 1010"));
    EXPECT_TRUE(containsLine(out, "tx_end 2 2 1040"));

    // parent_of relation: flit -> root
    EXPECT_TRUE(containsLine(out, "tx_relation \"parent_of\" 2 1"));
}

// ==================================================================
// Events as zero-duration child transactions
// ==================================================================

TEST(TxTraceTest, EventsAreZeroDurationChildren)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid = trace->addStream("seq0", "Sequencer");
    uint64_t gen = trace->addGeneratorPair(sid, "memreq");
    uint64_t evt_gen = gen + 1; // .events companion

    TraceId root = trace->createRootTransaction(gen, 1000);

    // Stamp two events at different ticks
    trace->stampEvent(root, evt_gen, "enqueue",
                      "system.ruby.l1_cntrl0.mandatoryQueue", 1005,
                      {
                          {"occupancy", uint64_t(3)},
                          {"vnet", uint64_t(0)},
                      });

    trace->stampEvent(root, evt_gen, "dequeue",
                      "system.ruby.l1_cntrl0.mandatoryQueue", 1010);

    trace->retireTransaction(root, 1045);

    std::string out = ss->str();

    // Event 1: begin == end (zero duration), on events generator
    EXPECT_TRUE(containsLine(out, "tx_begin 2 1 1005"));
    EXPECT_TRUE(containsLine(
        out, "tx_record_attribute 2 \"event_kind\" STRING = \"enqueue\""));
    EXPECT_TRUE(containsLine(out,
                             "tx_record_attribute 2 \"object_name\" STRING = "
                             "\"system.ruby.l1_cntrl0.mandatoryQueue\""));
    EXPECT_TRUE(
        containsLine(out, "tx_record_attribute 2 \"occupancy\" UNSIGNED = 3"));
    EXPECT_TRUE(containsLine(out, "tx_relation \"parent_of\" 2 1"));
    EXPECT_TRUE(containsLine(out, "tx_end 2 1 1005"));

    // Event 2
    EXPECT_TRUE(containsLine(out, "tx_begin 3 1 1010"));
    EXPECT_TRUE(containsLine(
        out, "tx_record_attribute 3 \"event_kind\" STRING = \"dequeue\""));
    EXPECT_TRUE(containsLine(out, "tx_end 3 1 1010"));
}

// ==================================================================
// IDs are globally unique and monotonically increasing
// ==================================================================

TEST(TxTraceTest, MonotonicUniqueIds)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid = trace->addStream("seq0", "Sequencer");
    uint64_t gen = trace->addGeneratorPair(sid, "memreq");
    uint64_t flit_gen = trace->addGeneratorPair(sid, "flit");

    TraceId t1 = trace->createRootTransaction(gen, 100);
    TraceId t2 = trace->createChildTransaction(flit_gen, t1, 110);
    TraceId t3 = trace->createRootTransaction(gen, 200);

    EXPECT_EQ(t1, 1u);
    EXPECT_EQ(t2, 2u);
    EXPECT_EQ(t3, 3u);

    trace->retireTransaction(t2, 130);
    trace->retireTransaction(t1, 150);
    trace->retireTransaction(t3, 250);
}

// ==================================================================
// End-to-end Ruby request scenario
// ==================================================================

TEST(TxTraceTest, EndToEndRubyRequestScenario)
{
    auto [trace, ss] = makeTrace();

    // Set up streams and generators for a 2-core system
    uint64_t s0 =
        trace->addStream("system.ruby.l1_cntrl0.sequencer", "Sequencer");
    uint64_t memreq0 = trace->addGeneratorPair(s0, "memreq");
    uint64_t memreq0_evt = memreq0 + 1;
    uint64_t flit0 = trace->addGeneratorPair(s0, "flit");
    uint64_t flit0_evt = flit0 + 1;

    // 1. Root request enters Ruby
    TraceId req =
        trace->createRootTransaction(memreq0, 1000,
                                     {
                                         {"address", uint64_t(0x80001000)},
                                         {"type", std::string("LD")},
                                         {"size", uint64_t(64)},
                                         {"requestor_id", uint64_t(0)},
                                     });

    // 2. Port crossing event
    trace->stampEvent(req, memreq0_evt, "port_crossing",
                      "system.ruby.l1_cntrl0.sequencer", 1000);

    // 3. Enqueue into mandatory queue
    trace->stampEvent(req, memreq0_evt, "enqueue",
                      "system.ruby.l1_cntrl0.mandatoryQueue", 1005,
                      {{"occupancy", uint64_t(1)}});

    // 4. Issue event
    trace->stampEvent(req, memreq0_evt, "issue",
                      "system.ruby.l1_cntrl0.sequencer", 1010);

    // 5. Flitisization: 2 flits created
    TraceId f0 =
        trace->createChildTransaction(flit0, req, 1015,
                                      {
                                          {"flit_index", uint64_t(0)},
                                          {"packet_id", uint64_t(100)},
                                          {"vnet", uint64_t(2)},
                                      });
    TraceId f1 =
        trace->createChildTransaction(flit0, req, 1015,
                                      {
                                          {"flit_index", uint64_t(1)},
                                          {"packet_id", uint64_t(100)},
                                          {"vnet", uint64_t(2)},
                                      });

    // 6. Flit 0 traverses router stages
    trace->stampEvent(f0, flit0_evt, "router_arrive",
                      "system.ruby.network.routers0", 1020,
                      {{"vc", uint64_t(1)}});
    trace->stampEvent(f0, flit0_evt, "switch_alloc",
                      "system.ruby.network.routers0", 1021);
    trace->stampEvent(f0, flit0_evt, "switch_traverse",
                      "system.ruby.network.routers0", 1022);
    trace->stampEvent(f0, flit0_evt, "link_traverse",
                      "system.ruby.network.int_links0.network_link", 1023);

    // 7. Flits retire at destination NI
    trace->retireTransaction(f0, 1030);
    trace->retireTransaction(f1, 1032);

    // 8. Completion callback
    trace->stampEvent(req, memreq0_evt, "completion",
                      "system.ruby.l1_cntrl0.sequencer", 1040,
                      {{"hit_type", std::string("L2_HIT")}});

    // 9. Root retires
    trace->retireTransaction(req, 1045);

    std::string out = ss->str();

    // Verify structure: 1 root + 2 flits + 8 events = 11 transactions
    // Relations: 2 flit-to-root + 8 event-to-parent = 10
    EXPECT_EQ(countMatches(out, "tx_begin"), 11);
    EXPECT_EQ(countMatches(out, "tx_end"), 11);
    EXPECT_EQ(countMatches(out, "tx_relation \"parent_of\""), 10);

    // Root request spans the full lifetime
    EXPECT_TRUE(containsLine(out, "tx_begin 1 0 1000"));
    EXPECT_TRUE(containsLine(out, "tx_end 1 0 1045"));

    // Both flits are children of root
    EXPECT_TRUE(containsLine(out, "tx_relation \"parent_of\" 3 1"));
    EXPECT_TRUE(containsLine(out, "tx_relation \"parent_of\" 4 1"));

    // Router stage events exist on flit transactions
    EXPECT_EQ(countMatches(out, "\"router_arrive\""), 1);
    EXPECT_EQ(countMatches(out, "\"switch_alloc\""), 1);
    EXPECT_EQ(countMatches(out, "\"switch_traverse\""), 1);
    EXPECT_EQ(countMatches(out, "\"link_traverse\""), 1);

    // Completion event carries hit type
    EXPECT_EQ(countMatches(out, "\"L2_HIT\""), 1);
}

// ==================================================================
// Text writer attribute types
// ==================================================================

TEST(TxTextWriterTest, AllAttributeTypes)
{
    auto ss = std::make_unique<std::stringstream>();
    auto *ss_ptr = ss.get();
    TxTextWriter writer(std::move(ss));

    writer.writeAttribute(1, "flag", AttrValue{true});
    writer.writeAttribute(1, "count", AttrValue{int64_t(-42)});
    writer.writeAttribute(1, "addr", AttrValue{uint64_t(4096)});
    writer.writeAttribute(1, "rate", AttrValue{double(0.95)});
    writer.writeAttribute(1, "cmd", AttrValue{std::string("ReadReq")});

    std::string out = ss_ptr->str();

    EXPECT_TRUE(
        containsLine(out, "tx_record_attribute 1 \"flag\" BOOLEAN = true"));
    EXPECT_TRUE(
        containsLine(out, "tx_record_attribute 1 \"count\" INTEGER = -42"));
    EXPECT_TRUE(
        containsLine(out, "tx_record_attribute 1 \"addr\" UNSIGNED = 4096"));
    EXPECT_TRUE(containsLine(
        out, "tx_record_attribute 1 \"rate\" FLOATING_POINT_NUMBER = 0.95"));
    EXPECT_TRUE(containsLine(
        out, "tx_record_attribute 1 \"cmd\" STRING = \"ReadReq\""));
}

// ==================================================================
// Empty stream kind
// ==================================================================

TEST(TxTextWriterTest, EmptyStreamKind)
{
    auto ss = std::make_unique<std::stringstream>();
    auto *ss_ptr = ss.get();
    TxTextWriter writer(std::move(ss));

    writer.writeStream(0, "test_stream", "");

    EXPECT_TRUE(containsLine(ss_ptr->str(),
                             "scv_tr_stream (ID 0, name \"test_stream\","
                             " kind \"<no_stream_kind>\")"));
}

// ==================================================================
// reserveId returns unique monotonic IDs
// ==================================================================

TEST(TxTraceTest, ReserveIdMonotonic)
{
    auto [trace, ss] = makeTrace();

    TraceId a = trace->reserveId();
    TraceId b = trace->reserveId();
    TraceId c = trace->reserveId();

    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_EQ(c, 3u);

    // Reserving IDs should not produce any output
    EXPECT_EQ(ss->str(), "");
}

// ==================================================================
// createRootTransactionWithId uses a pre-reserved ID
// ==================================================================

TEST(TxTraceTest, CreateRootTransactionWithId)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid = trace->addStream("seq0", "Sequencer");
    uint64_t gen = trace->addGeneratorPair(sid, "memreq");

    TraceId reserved = trace->reserveId();
    EXPECT_EQ(reserved, 1u);

    // Nothing written yet for this transaction
    std::string before = ss->str();
    EXPECT_EQ(countMatches(before, "tx_begin"), 0);

    trace->createRootTransactionWithId(reserved, gen, 1000,
                                       {{"address", uint64_t(0x80001000)}});
    trace->retireTransaction(reserved, 1045);

    std::string out = ss->str();

    EXPECT_TRUE(containsLine(out, "tx_begin 1 0 1000"));
    EXPECT_TRUE(containsLine(
        out, "tx_record_attribute 1 \"address\" UNSIGNED = 2147487744"));
    EXPECT_TRUE(containsLine(out, "tx_end 1 0 1045"));
}

// ==================================================================
// createChildTransactionWithId uses a pre-reserved ID
// ==================================================================

TEST(TxTraceTest, CreateChildTransactionWithId)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid = trace->addStream("seq0", "Sequencer");
    uint64_t memreq_gen = trace->addGeneratorPair(sid, "memreq");
    uint64_t flit_gen = trace->addGeneratorPair(sid, "flit");

    TraceId root = trace->createRootTransaction(memreq_gen, 1000);

    TraceId flit_id = trace->reserveId();
    trace->createChildTransactionWithId(flit_id, flit_gen, root, 1010,
                                        {{"flit_index", uint64_t(0)}});
    trace->retireTransaction(flit_id, 1040);
    trace->retireTransaction(root, 1045);

    std::string out = ss->str();

    // Flit child transaction with reserved ID
    EXPECT_TRUE(containsLine(out, "tx_begin 2 2 1010"));
    EXPECT_TRUE(containsLine(out, "tx_end 2 2 1040"));
    EXPECT_TRUE(containsLine(out, "tx_relation \"parent_of\" 2 1"));
}

// ==================================================================
// reserveId interleaves correctly with allocateId
// ==================================================================

TEST(TxTraceTest, ReserveIdInterleaveWithCreate)
{
    auto [trace, ss] = makeTrace();

    uint64_t sid = trace->addStream("seq0", "Sequencer");
    uint64_t gen = trace->addGeneratorPair(sid, "memreq");

    // Reserve an ID first
    TraceId reserved = trace->reserveId();
    EXPECT_EQ(reserved, 1u);

    // Create a normal root — should get the next ID
    TraceId root = trace->createRootTransaction(gen, 100);
    EXPECT_EQ(root, 2u);

    // Now finalize the reserved ID
    trace->createRootTransactionWithId(reserved, gen, 50);

    trace->retireTransaction(root, 200);
    trace->retireTransaction(reserved, 150);

    std::string out = ss->str();
    EXPECT_TRUE(containsLine(out, "tx_begin 2 0 100"));
    EXPECT_TRUE(containsLine(out, "tx_begin 1 0 50"));
}

} // anonymous namespace
