/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_merge_cursors.h"

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/query_request.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using ResponseStatus = executor::TaskExecutor::ResponseStatus;

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

const NamespaceString kTestNss = NamespaceString("test.mergeCursors"_sd);
const HostAndPort kTestHost = HostAndPort("localhost:27017"_sd);

const CursorId kExhaustedCursorID = 0;

class DocumentSourceMergeCursorsTest : public ShardingTestFixture {
public:
    DocumentSourceMergeCursorsTest() : _expCtx(new ExpressionContextForTest(kTestNss)) {}

    void setUp() override {
        ShardingTestFixture::setUp();
        setRemote(HostAndPort("ClientHost", 12345));

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;
        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());

            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                stdx::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

    boost::intrusive_ptr<ExpressionContextForTest> getExpCtx() {
        return _expCtx.get();
    }

private:
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectNonArray) {
    auto spec = BSON("$mergeCursors" << BSON(
                         "cursors" << BSON_ARRAY(BSON("ns" << kTestNss.ns() << "id" << 0LL << "host"
                                                           << kTestHost.toString()))));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17026);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectEmptyArray) {
    auto spec = BSON("$mergeCursors" << BSONArray());
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50729);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectCursorWithNoNamespace) {
    auto spec =
        BSON("$mergeCursors" << BSON_ARRAY(BSON("id" << 0LL << "host" << kTestHost.toString())));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50731);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectCursorWithNonStringNamespace) {
    auto spec = BSON("$mergeCursors" << BSON_ARRAY(
                         BSON("ns" << 4 << "id" << 0LL << "host" << kTestHost.toString())));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50731);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectCursorsWithDifferentNamespaces) {
    auto spec = BSON(
        "$mergeCursors"
        << BSON_ARRAY(BSON("ns" << kTestNss.ns() << "id" << 0LL << "host" << kTestHost.toString())
                      << BSON("ns"
                              << "test.other"_sd
                              << "id"
                              << 0LL
                              << "host"
                              << kTestHost.toString())));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50720);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectCursorWithNoHost) {
    auto spec = BSON("$mergeCursors" << BSON_ARRAY(BSON("ns" << kTestNss.ns() << "id" << 0LL)));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50721);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectCursorWithNonStringHost) {
    auto spec = BSON("$mergeCursors"
                     << BSON_ARRAY(BSON("ns" << kTestNss.ns() << "id" << 0LL << "host" << 4LL)));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50721);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectCursorWithNonLongId) {
    auto spec = BSON("$mergeCursors" << BSON_ARRAY(BSON("ns" << kTestNss.ns() << "id"
                                                             << "zero"
                                                             << "host"
                                                             << kTestHost.toString())));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50722);
    spec = BSON("$mergeCursors" << BSON_ARRAY(
                    BSON("ns" << kTestNss.ns() << "id" << 0 << "host" << kTestHost.toString())));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50722);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectCursorWithExtraField) {
    auto spec =
        BSON("$mergeCursors" << BSON_ARRAY(BSON(
                 "ns" << kTestNss.ns() << "id" << 0LL << "host" << kTestHost.toString() << "extra"
                      << "unexpected")));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       50730);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldBeAbleToParseTheSerializedVersionOfItself) {
    auto spec =
        BSON("$mergeCursors" << BSON_ARRAY(
                 BSON("ns" << kTestNss.ns() << "id" << 1LL << "host" << kTestHost.toString())
                 << BSON("ns" << kTestNss.ns() << "id" << 2LL << "host" << kTestHost.toString())));
    auto mergeCursors =
        DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx());
    std::vector<Value> serializationArray;
    mergeCursors->serializeToArray(serializationArray);
    ASSERT_EQ(serializationArray.size(), 1UL);
    // The serialized version might not be identical to 'spec', the fields might be in a different
    // order, etc. Here we just make sure that the final parse doesn't throw.
    auto newSpec = serializationArray[0].getDocument().toBson();
    ASSERT(DocumentSourceMergeCursors::createFromBson(newSpec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldReportEOFWithNoCursors) {
    auto expCtx = getExpCtx();
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, kExhaustedCursorID, {}));
    cursors.emplace_back(
        kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, kExhaustedCursorID, {}));
    auto pipeline = uassertStatusOK(Pipeline::create({}, expCtx));
    auto mergeCursorsStage =
        DocumentSourceMergeCursors::create(std::move(cursors), executor(), expCtx);

    ASSERT_TRUE(mergeCursorsStage->getNext().isEOF());
}

BSONObj cursorResponseObj(const NamespaceString& nss,
                          CursorId cursorId,
                          std::vector<BSONObj> batch) {
    return CursorResponse{nss, cursorId, std::move(batch)}.toBSON(
        CursorResponse::ResponseType::SubsequentResponse);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldBeAbleToIterateCursorsUntilEOF) {
    auto expCtx = getExpCtx();
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {}));
    cursors.emplace_back(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {}));
    auto pipeline = uassertStatusOK(Pipeline::create({}, expCtx));
    pipeline->addInitialSource(
        DocumentSourceMergeCursors::create(std::move(cursors), executor(), expCtx));

    // Iterate the $mergeCursors stage asynchronously on a different thread, since it will block
    // waiting for network responses, which we will manually schedule below.
    auto future = launchAsync([&pipeline]() {
        for (int i = 0; i < 5; ++i) {
            ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 1}}));
        }
        ASSERT_FALSE(static_cast<bool>(pipeline->getNext()));
    });


    // Schedule responses to two getMores which keep the cursor open.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(
            expCtx->ns, request.cmdObj["getMore"].Long(), {BSON("x" << 1), BSON("x" << 1)});
    });
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(
            expCtx->ns, request.cmdObj["getMore"].Long(), {BSON("x" << 1), BSON("x" << 1)});
    });

    // Schedule responses to two getMores which report the cursor is exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->ns, kExhaustedCursorID, {});
    });
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->ns, kExhaustedCursorID, {BSON("x" << 1)});
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldNotKillCursorsIfNeverIterated) {
    auto expCtx = getExpCtx();
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {}));
    cursors.emplace_back(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {}));
    auto pipeline = uassertStatusOK(Pipeline::create({}, expCtx));
    pipeline->addInitialSource(
        DocumentSourceMergeCursors::create(std::move(cursors), executor(), expCtx));

    pipeline.reset();  // Delete the pipeline before using it.

    network()->enterNetwork();
    ASSERT_FALSE(network()->hasReadyRequests());
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldKillCursorIfPartiallyIterated) {
    auto expCtx = getExpCtx();
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {}));
    auto pipeline = uassertStatusOK(Pipeline::create({}, expCtx));
    pipeline->addInitialSource(
        DocumentSourceMergeCursors::create(std::move(cursors), executor(), expCtx));

    // Iterate the pipeline asynchronously on a different thread, since it will block waiting for
    // network responses, which we will manually schedule below.
    auto future = launchAsync([&pipeline]() {
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 1}}));
        pipeline.reset();  // Stop iterating and delete the pipeline.
    });

    // Note we do not use 'kExhaustedCursorID' here, so the cursor is still open.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->ns, 1, {BSON("x" << 1), BSON("x" << 1)});
    });

    // Here we're looking for the killCursors request to be scheduled.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["killCursors"]);
        auto cursors = request.cmdObj["cursors"];
        ASSERT_EQ(cursors.type(), BSONType::Array);
        auto cursorsArray = cursors.Array();
        ASSERT_FALSE(cursorsArray.empty());
        auto cursorId = cursorsArray[0].Long();
        ASSERT(cursorId == 1);
        // The ARM doesn't actually inspect the response of the killCursors, so we don't have to put
        // anything except {ok: 1}.
        return BSON("ok" << 1);
    });
    future.timed_get(kFutureTimeout);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldOptimizeWithASortToEnsureCorrectOrder) {
    auto expCtx = getExpCtx();

    // Make a pipeline with a single $sort stage that is merging pre-sorted results.
    const bool mergingPresorted = true;
    const long long noLimit = -1;
    auto sortStage = DocumentSourceSort::create(expCtx,
                                                BSON("x" << 1),
                                                noLimit,
                                                DocumentSourceSort::kMaxMemoryUsageBytes,
                                                mergingPresorted);
    auto pipeline = uassertStatusOK(Pipeline::create({std::move(sortStage)}, expCtx));

    // Make a $mergeCursors stage and add it to the front of the pipeline.
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {}));
    cursors.emplace_back(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {}));
    auto mergeCursorsStage =
        DocumentSourceMergeCursors::create(std::move(cursors), executor(), expCtx);
    pipeline->addInitialSource(std::move(mergeCursorsStage));

    // After optimization we should only have a $mergeCursors stage.
    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->getSources().size(), 1UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    // Iterate the pipeline asynchronously on a different thread, since it will block waiting for
    // network responses, which we will manually schedule below.
    auto future = launchAsync([&pipeline]() {
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 1}}));
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 2}}));
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 3}}));
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 4}}));
        ASSERT_FALSE(static_cast<bool>(pipeline->getNext()));
    });

    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->ns,
                                 kExhaustedCursorID,
                                 {BSON("x" << 1 << "$sortKey" << BSON("" << 1)),
                                  BSON("x" << 3 << "$sortKey" << BSON("" << 3))});
    });
    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->ns,
                                 kExhaustedCursorID,
                                 {BSON("x" << 2 << "$sortKey" << BSON("" << 2)),
                                  BSON("x" << 4 << "$sortKey" << BSON("" << 4))});
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldNotRemoveLimitWhenOptimizingWithLeadingSort) {
    auto expCtx = getExpCtx();

    // Make a pipeline with a single $sort stage that is merging pre-sorted results.
    const bool mergingPresorted = true;
    const long long limit = 3;
    auto sortStage = DocumentSourceSort::create(
        expCtx, BSON("x" << 1), limit, DocumentSourceSort::kMaxMemoryUsageBytes, mergingPresorted);
    auto pipeline = uassertStatusOK(Pipeline::create({std::move(sortStage)}, expCtx));

    // Make a $mergeCursors stage and add it to the front of the pipeline.
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {}));
    cursors.emplace_back(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {}));
    auto mergeCursorsStage =
        DocumentSourceMergeCursors::create(std::move(cursors), executor(), expCtx);
    pipeline->addInitialSource(std::move(mergeCursorsStage));

    // After optimization, we should still have a $limit stage.
    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->getSources().size(), 2UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));
    ASSERT_TRUE(dynamic_cast<DocumentSourceLimit*>(pipeline->getSources().back().get()));

    // Iterate the pipeline asynchronously on a different thread, since it will block waiting for
    // network responses, which we will manually schedule below.
    auto future = launchAsync([&]() {
        for (int i = 1; i <= limit; ++i) {
            ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", i}}));
        }
        ASSERT_FALSE(static_cast<bool>(pipeline->getNext()));
    });

    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->ns,
                                 kExhaustedCursorID,
                                 {BSON("x" << 1 << "$sortKey" << BSON("" << 1)),
                                  BSON("x" << 3 << "$sortKey" << BSON("" << 3))});
    });
    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->ns,
                                 kExhaustedCursorID,
                                 {BSON("x" << 2 << "$sortKey" << BSON("" << 2)),
                                  BSON("x" << 4 << "$sortKey" << BSON("" << 4))});
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldSerializeSortIfAbsorbedViaOptimize) {
    auto expCtx = getExpCtx();

    // Make a pipeline with a single $sort stage that is merging pre-sorted results.
    const bool mergingPresorted = true;
    const long long limit = 3;
    auto sortStage = DocumentSourceSort::create(
        expCtx, BSON("x" << 1), limit, DocumentSourceSort::kMaxMemoryUsageBytes, mergingPresorted);
    auto pipeline = uassertStatusOK(Pipeline::create({std::move(sortStage)}, expCtx));

    // Make a $mergeCursors stage and add it to the front of the pipeline.
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors;
    cursors.emplace_back(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {}));
    cursors.emplace_back(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {}));
    auto mergeCursorsStage =
        DocumentSourceMergeCursors::create(std::move(cursors), executor(), expCtx);
    pipeline->addInitialSource(std::move(mergeCursorsStage));

    // After optimization, we should still have a $limit stage.
    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->getSources().size(), 2UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));
    ASSERT_TRUE(dynamic_cast<DocumentSourceLimit*>(pipeline->getSources().back().get()));

    auto serialized = pipeline->serialize();
    ASSERT_EQ(serialized.size(), 3UL);
    ASSERT_FALSE(serialized[0]["$mergeCursors"].missing());
    ASSERT_FALSE(serialized[1]["$sort"].missing());
    ASSERT_FALSE(serialized[2]["$limit"].missing());
}

}  // namespace
}  // namespace mongo
