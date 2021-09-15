#ifndef TM_KIT_TRANSPORT_GRPC_INTEROP_GRPC_CLIENT_FACILITY_HPP_
#define TM_KIT_TRANSPORT_GRPC_INTEROP_GRPC_CLIENT_FACILITY_HPP_

#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/AppClassifier.hpp>

#include <tm_kit/transport/grpc_interop/GrpcInteropComponent.hpp>
#include <tm_kit/transport/grpc_interop/GrpcServiceInfo.hpp>
#include <tm_kit/transport/grpc_interop/GrpcSerializationHelper.hpp>

#include <type_traits>
#include <memory>
#include <iostream>
#include <sstream>
#include <mutex>
#include <condition_variable>

#include <grpcpp/channel.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/sync_stream.h>
#include <grpcpp/impl/codegen/client_unary_call.h>
#include <grpcpp/impl/codegen/client_callback.h>

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace grpc_interop {

    template <class M, typename=std::enable_if_t<
        infra::app_classification_v<M> == infra::AppClassification::RealTime
        &&
        std::is_convertible_v<
            typename M::EnvironmentType *
            , GrpcInteropComponent *
        >
    >>
    class GrpcClientFacilityFactory {
    private:
        using Env = typename M::EnvironmentType;
    public:
        template <class Req, class Resp, typename=std::enable_if_t<
            basic::bytedata_utils::ProtobufStyleSerializableChecker<Req>::IsProtobufStyleSerializable()
            &&
            basic::bytedata_utils::ProtobufStyleSerializableChecker<Resp>::IsProtobufStyleSerializable()
        >>
        static auto createClientFacility(
            std::string const &host
            , int port
            , GrpcServiceInfo const &serviceInfo
            , bool isSingleCallbackServer = false
        )
            -> std::shared_ptr<
                typename M::template OnOrderFacility<
                    Req, Resp
                >
            >
        {
            class LocalF : 
                public M::IExternalComponent
                , public M::template AbstractOnOrderFacility<Req, Resp> 
                , public infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                    typename M::template Key<Req>
                    , LocalF 
                >
            {
            private:
                bool isSingleCallback_;
                std::string serviceInfoStr_;
                std::string channelAddr_;
                std::shared_ptr<grpc::Channel> channel_;
                std::mutex startMutex_;
                std::condition_variable startCond_;
                class OneCallReactor : public grpc::ClientReadReactor<Resp> {
                private:
                    LocalF *parent_;
                    Env *env_;
                    typename Env::IDType id_;
                    Req req_;
                    Resp resp_;
                    grpc::ClientContext ctx_;
                    bool singleCallback_;
                public:
                    OneCallReactor(LocalF *parent, Env *env, typename Env::IDType const &id, Req &&req, bool singleCallback)
                        : parent_(parent), env_(env), id_(id), req_(std::move(req)), resp_(), ctx_(), singleCallback_(singleCallback)
                    {}
                    ~OneCallReactor() = default;
                    void start() {
                        grpc::ClientReadReactor<Resp>::StartCall();
                        grpc::ClientReadReactor<Resp>::StartRead(&resp_);
                    }
                    virtual void OnReadDone(bool good) override final {
                        if (good) {
                            parent_->publish(
                                env_
                                , typename M::template Key<Resp> {
                                    id_ 
                                    , std::move(resp_)
                                }
                                , singleCallback_
                            );
                            if (!singleCallback_) {
                                grpc::ClientReadReactor<Resp>::StartRead(&resp_);
                            }
                        }
                    }
                    virtual void OnDone(grpc::Status const &) {
                        parent_->eraseReactor(id_);
                    }
                    grpc::ClientContext *context() {
                        return &ctx_;
                    }
                    Req const *req() const {
                        return &req_;
                    }
                };
                std::unordered_map<typename Env::IDType, std::unique_ptr<OneCallReactor>> reactorMap_;
                std::mutex reactorMapMutex_;
            public:
                LocalF(std::string const &host, int port, GrpcServiceInfo const &serviceInfo, bool isSingleCallback)
                    : M::IExternalComponent()
                    , M::template AbstractOnOrderFacility<Req, Resp>()
                    , infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                        typename M::template Key<Req>
                        , LocalF 
                    >()
                    , isSingleCallback_(isSingleCallback)
                    , serviceInfoStr_(grpcServiceInfoAsEndPointString(serviceInfo))
                    , channelAddr_()
                    , channel_()
                    , reactorMap_()
                    , reactorMapMutex_()
                {
                    std::ostringstream oss;
                    oss << host << ":" << port;
                    channelAddr_ = oss.str();
                }
                virtual void start(Env *env) override final {
                    std::lock_guard<std::mutex> _(startMutex_);
                    channel_ = static_cast<GrpcInteropComponent *>(env)->grpc_interop_getChannel(channelAddr_);
                    startCond_.notify_one();
                }
                void actuallyHandle(typename M::template InnerData<typename M::template Key<Req>> &&req) {
                    std::lock_guard<std::mutex> _(reactorMapMutex_);
                    auto iter = reactorMap_.find(req.timedData.value.id());
                    if (iter != reactorMap_.end()) {
                        //Ignore client side duplicate request
                        return;
                    }
                    iter = reactorMap_.insert({
                        req.timedData.value.id()
                        , std::make_unique<OneCallReactor>(
                            this 
                            , req.environment 
                            , req.timedData.value.id()
                            , std::move(req.timedData.value.key())
                            , isSingleCallback_
                        )
                    }).first;
                    grpc::internal::ClientCallbackReaderFactory<Resp>::Create(
                        channel_.get()
                        , grpc::internal::RpcMethod {
                            serviceInfoStr_.c_str()
                            , (
                                isSingleCallback_
                                ?
                                grpc::internal::RpcMethod::RpcType::NORMAL_RPC
                                :
                                grpc::internal::RpcMethod::RpcType::SERVER_STREAMING
                            )
                        }
                        , iter->second->context()
                        , iter->second->req()
                        , iter->second.get()
                    );
                    iter->second->start();
                }
                //the signature requires a copy, because the ID 
                //was passed from a struct that will be erased
                void eraseReactor(typename Env::IDType id) {
                    this->markEndHandlingRequest(id);
                    {
                        std::lock_guard<std::mutex> _(reactorMapMutex_);
                        reactorMap_.erase(id);
                    }
                }
                void idleWork() {}
                void waitForStart() {
                    while (true) {
                        std::unique_lock<std::mutex> lock(startMutex_);
                        startCond_.wait_for(lock, std::chrono::milliseconds(1));
                        if (channel_) {
                            lock.unlock();
                            break;
                        } else {
                            lock.unlock();
                        }
                    }
                }
            };
            return M::template fromAbstractOnOrderFacility<Req,Resp>(new LocalF(host, port, serviceInfo, isSingleCallbackServer));
        }
        template <class Req, class Resp, typename=std::enable_if_t<
            basic::bytedata_utils::ProtobufStyleSerializableChecker<Req>::IsProtobufStyleSerializable()
            &&
            basic::bytedata_utils::ProtobufStyleSerializableChecker<Resp>::IsProtobufStyleSerializable()
        >>
        static std::vector<Resp> runSyncClient(
            std::string const &host
            , int port
            , GrpcServiceInfo const &serviceInfo
            , Req const &req
            , bool isSingleCallbackServer = false
        )
        {
            std::ostringstream oss;
            oss << host << ":" << port;
            auto channel = grpc::CreateChannel(oss.str(), grpc::InsecureChannelCredentials());
            std::vector<Resp> ret;
            grpc::ClientContext ctx;
            auto serviceInfoStr = grpcServiceInfoAsEndPointString(serviceInfo);
            if (isSingleCallbackServer) {
                Resp resp;
                if (grpc::internal::BlockingUnaryCall<Req, Resp>(
                    channel.get()
                    , grpc::internal::RpcMethod {
                        serviceInfoStr.c_str()
                        , grpc::internal::RpcMethod::RpcType::NORMAL_RPC
                    }
                    , &ctx 
                    , req 
                    , &resp
                ).ok()) {
                    ret.push_back(resp);
                }
            } else {
                std::unique_ptr<grpc::ClientReader<Resp>> reader {
                    grpc::internal::ClientReaderFactory<Resp>::Create(
                        channel.get() 
                        , grpc::internal::RpcMethod {
                            serviceInfoStr.c_str()
                            , grpc::internal::RpcMethod::RpcType::SERVER_STREAMING
                        }
                        , &ctx 
                        , req
                    )
                };
                Resp resp;
                while (reader->Read(&resp)) {
                    ret.push_back(std::move(resp));
                }
                reader->Finish();
            }
            return ret;
        }
    };

}}}}}

#endif