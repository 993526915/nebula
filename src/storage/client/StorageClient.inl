/* Copyright (c) 2018 - present, VE Software Inc. All rights reserved
 *
 * This source code is licensed under Apache 2.0 License
 *  (found in the LICENSE.Apache file in the root directory)
 */

#include "thrift/ThriftClientManager.h"
#include <folly/Try.h>

namespace nebula {
namespace storage {

namespace detail {

template<class Request, class RemoteFunc, class Response>
struct ResponseContext {
public:
    ResponseContext(size_t partsSent, RemoteFunc&& remoteFunc)
        : resp(partsSent)
        , serverMethod(std::move(remoteFunc)) {}

    // Return true if processed all responses
    bool finishSending() {
        std::lock_guard<std::mutex> g(lock_);
        finishSending_ = true;
        if (ongoingRequests_.empty() && !fulfilled_) {
            fulfilled_ = true;
            return true;
        } else {
            return false;
        }
    }

    std::pair<const Request&, bool> insertRequest(HostAddr host, Request&& req) {
        std::lock_guard<std::mutex> g(lock_);
        auto res = ongoingRequests_.emplace(host, std::move(req));
        return std::make_pair(res.first->second, res.second);
    }

    const Request& findRequest(HostAddr host) {
        std::lock_guard<std::mutex> g(lock_);
        auto it = ongoingRequests_.find(host);
        DCHECK(it != ongoingRequests_.end());
        return it->second;
    }

    // Return true if processed all responses
    bool removeRequest(HostAddr host) {
        std::lock_guard<std::mutex> g(lock_);
        ongoingRequests_.erase(host);
        if (finishSending_ && !fulfilled_ && ongoingRequests_.empty()) {
            fulfilled_ = true;
            return true;
        } else {
            return false;
        }
    }


public:
    folly::Promise<StorageRpcResponse<Response>> promise;
    StorageRpcResponse<Response> resp;
    RemoteFunc serverMethod;

private:
    std::mutex lock_;
    std::unordered_map<HostAddr, Request> ongoingRequests_;
    bool finishSending_{false};
    bool fulfilled_{false};
};

}  // namespace detail


template<class Request, class RemoteFunc, class Response>
folly::SemiFuture<StorageRpcResponse<Response>> StorageClient::collectResponse(
        folly::EventBase* evb,
        std::unordered_map<HostAddr, Request> requests,
        RemoteFunc&& remoteFunc) {
    auto context = std::make_shared<detail::ResponseContext<Request, RemoteFunc, Response>>(
        requests.size(), std::move(remoteFunc));

    if (evb == nullptr) {
        DCHECK(!!ioThreadPool_);
        evb = ioThreadPool_->getEventBase();
    }

    for (auto& req : requests) {
        auto& host = req.first;
        auto client = thrift::ThriftClientManager<storage::cpp2::StorageServiceAsyncClient>
                            ::getClient(host, evb);
        // Result is a pair of <Request&, bool>
        auto res = context->insertRequest(host, std::move(req.second));
        DCHECK(res.second);
        // Invoke the remote method
        context->serverMethod(client.get(), res.first)
            // Future process code will be executed on the IO thread
            // Since all requests are sent using the same eventbase, all then-callback
            // will be executed on the same IO thread
            .then(evb, [context, host] (folly::Try<Response>&& val) {
                auto& r = context->findRequest(host);
                if (val.hasException()) {
                    for (auto& part : r.parts) {
                        context->resp.failedParts().emplace(
                            part.first,
                            storage::cpp2::ErrorCode::E_RPC_FAILURE);
                    }
                    context->resp.markFailure();
                } else {
                    auto resp = std::move(val.value());
                    auto& result = resp.get_result();
                    bool hasFailure{false};
                    for (auto& code : result.get_failed_codes()) {
                        hasFailure = true;
                        if (code.get_code() == storage::cpp2::ErrorCode::E_LEADER_CHANGED) {
                            // TODO Need to retry the new leader
                            LOG(FATAL) << "Not implmented";
                        } else {
                            // Simply keep the result
                            context->resp.failedParts().emplace(code.get_part_id(),
                                                                code.get_code());
                        }
                    }
                    if (hasFailure) {
                        context->resp.markFailure();
                    }

                    // Adjust the latency
                    context->resp.setLatency(result.get_latency_in_ms());

                    // Keep the response
                    context->resp.responses().emplace_back(std::move(resp));
                }

                if (context->removeRequest(host)) {
                    // Received all responses
                    context->promise.setValue(std::move(context->resp));
                }
            });
    }

    if (context->finishSending()) {
        // Received all responses, most likely, all rpc failed
        context->promise.setValue(std::move(context->resp));
    }

    return context->promise.getSemiFuture();
}

}   // namespace storage
}   // namespace nebula
