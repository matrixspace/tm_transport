#ifndef TM_KIT_TRANSPORT_JSON_REST_JSON_REST_CLIENT_FACILITY_HPP_
#define TM_KIT_TRANSPORT_JSON_REST_JSON_REST_CLIENT_FACILITY_HPP_

#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/infra/AppClassifier.hpp>

#include <tm_kit/basic/StructFieldInfoBasedCsvUtils.hpp>

#include <tm_kit/transport/json_rest/JsonRESTComponent.hpp>
#include <tm_kit/transport/json_rest/RawString.hpp>
#include <tm_kit/transport/json_rest/ComplexInput.hpp>
#include <tm_kit/transport/json_rest/JsonRESTClientFacilityUtils.hpp>
#include <tm_kit/transport/AbstractIdentityCheckerComponent.hpp>

#include <type_traits>
#include <memory>
#include <iostream>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <iomanip>

namespace dev { namespace cd606 { namespace tm { namespace transport { namespace json_rest {

    template <class M, typename=std::enable_if_t<
        infra::app_classification_v<M> == infra::AppClassification::RealTime
        &&
        std::is_convertible_v<
            typename M::EnvironmentType *
            , JsonRESTComponent *
        >
    >>
    class JsonRESTClientFacilityFactory {
    private:
        using Env = typename M::EnvironmentType;
        static auto createComplexInputClientFacility(
            ConnectionLocator const &locator
        )
            -> std::shared_ptr<
                typename M::template OnOrderFacility<
                    json_rest::ComplexInput, json_rest::RawStringWithStatus
                >
            >
        {
            class LocalF : 
                public M::IExternalComponent
                , public M::template AbstractOnOrderFacility<json_rest::ComplexInput, json_rest::RawStringWithStatus> 
                , public infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                    typename M::template Key<json_rest::ComplexInput>
                    , LocalF 
                >
            {
            private:
                ConnectionLocator locator_;
            public:
                LocalF(ConnectionLocator const &locator)
                    : M::IExternalComponent()
                    , M::template AbstractOnOrderFacility<json_rest::ComplexInput, json_rest::RawStringWithStatus>()
                    , infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                        typename M::template Key<json_rest::ComplexInput>
                        , LocalF 
                    >()
                    , locator_(locator)
                {
                    this->startThread();
                }
                virtual void start(Env *env) override final {
                }
                void actuallyHandle(typename M::template InnerData<typename M::template Key<json_rest::ComplexInput>> &&req) {
                    auto id = req.timedData.value.id();

                    std::ostringstream oss;
                    auto const &input = req.timedData.value.key();
                    
                    auto *env = req.environment;
                    ConnectionLocator updatedLocator = locator_.modifyIdentifier(input.path);
                    std::unordered_map<std::string, std::string> locatorHeaderProps;
                    if (!input.headers.empty()) {
                        for (auto const &item : input.headers) {
                            locatorHeaderProps.insert(std::make_pair("header/"+item.first, item.second));
                        }
                    }
                    if (input.auth_token) {
                        locatorHeaderProps.insert(std::make_pair("auth_token", *(input.auth_token)));
                    }
                    if (!locatorHeaderProps.empty()) {
                        updatedLocator = updatedLocator.addProperties(locatorHeaderProps);
                    }
                    
                    env->JsonRESTComponent::addJsonRESTClient(
                        updatedLocator
                        , std::string {input.query}
                        , std::string {input.body}
                        , [this,env,id=std::move(id)](unsigned status, std::string &&response) mutable {
                            this->publish(
                                env 
                                , typename M::template Key<json_rest::RawStringWithStatus> {
                                    std::move(id)
                                    , {status, std::move(response)}
                                }
                                , true
                            );
                        }
                        , input.contentType
                        , input.method
                    );
                }
                void idleWork() {}
                void waitForStart() {}
            };
            return M::template fromAbstractOnOrderFacility<json_rest::ComplexInput, json_rest::RawStringWithStatus>(new LocalF(locator));
        }
        template <class T>
        static auto createComplexInputWithDataClientFacility(
            ConnectionLocator const &locator
        )
            -> std::shared_ptr<
                typename M::template OnOrderFacility<
                    json_rest::ComplexInputWithData<T>, json_rest::RawStringWithStatus
                >
            >
        {
            class LocalF : 
                public M::IExternalComponent
                , public M::template AbstractOnOrderFacility<json_rest::ComplexInputWithData<T>, json_rest::RawStringWithStatus> 
                , public infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                    typename M::template Key<json_rest::ComplexInputWithData<T>>
                    , LocalF 
                >
            {
            private:
                ConnectionLocator locator_;
            public:
                LocalF(ConnectionLocator const &locator)
                    : M::IExternalComponent()
                    , M::template AbstractOnOrderFacility<json_rest::ComplexInputWithData<T>, json_rest::RawStringWithStatus>()
                    , infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                        typename M::template Key<json_rest::ComplexInputWithData<T>>
                        , LocalF 
                    >()
                    , locator_(locator)
                {
                    this->startThread();
                }
                virtual void start(Env *env) override final {
                }
                void actuallyHandle(typename M::template InnerData<typename M::template Key<json_rest::ComplexInputWithData<T>>> &&req) {
                    auto id = req.timedData.value.id();

                    std::ostringstream oss;
                    auto const &input = req.timedData.value.key();
                    
                    auto *env = req.environment;
                    ConnectionLocator updatedLocator = locator_.modifyIdentifier(input.path);
                    if (!input.headers.empty()) {
                        std::unordered_map<std::string, std::string> locatorHeaderProps;
                        for (auto const &item : input.headers) {
                            locatorHeaderProps.insert(std::make_pair("header/"+item.first, item.second));
                        }
                        updatedLocator = updatedLocator.addProperties(locatorHeaderProps);
                    }
                    
                    env->JsonRESTComponent::addJsonRESTClient(
                        updatedLocator
                        , std::string {input.query}
                        , std::string {input.body}
                        , [this,env,id=std::move(id)](unsigned status, std::string &&response) mutable {
                            this->publish(
                                env 
                                , typename M::template Key<json_rest::RawStringWithStatus> {
                                    std::move(id)
                                    , {status, std::move(response)}
                                }
                                , true
                            );
                        }
                        , input.contentType
                        , input.method
                    );
                }
                void idleWork() {}
                void waitForStart() {}
            };
            return M::template fromAbstractOnOrderFacility<json_rest::ComplexInputWithData<T>, json_rest::RawStringWithStatus>(new LocalF(locator));
        }
    public:
        template <class Req, class Resp>
        static auto createClientFacility(
            ConnectionLocator const &locator
        )
            -> std::shared_ptr<
                typename M::template OnOrderFacility<
                    Req, Resp
                >
            >
        {
            if constexpr (
                std::is_same_v<Req, json_rest::ComplexInput>
                &&
                std::is_same_v<Resp, json_rest::RawStringWithStatus>
            ) {
                return createComplexInputClientFacility(locator);
            } else if constexpr (
                json_rest::IsComplexInputWithData<Req>::value
                &&
                std::is_same_v<Resp, json_rest::RawStringWithStatus>
            ) {
                return createComplexInputWithDataClientFacility<
                    typename json_rest::IsComplexInputWithData<Req>::DataType
                >(locator);
            } else if constexpr (
                basic::nlohmann_json_interop::JsonWrappable<Req>::value
                &&
                basic::nlohmann_json_interop::JsonWrappable<Resp>::value
            ) {
                class LocalF : 
                    public M::IExternalComponent
                    , public M::template AbstractOnOrderFacility<Req, Resp> 
                    , public infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                        typename M::template Key<Req>
                        , LocalF 
                    >
                {
                private:
                    ConnectionLocator locator_;
                    std::string httpMethod_;
                    bool useGet_;
                    bool simplePost_;
                    bool otherwiseEncodeInUrl_;
                    bool noRequestResponseWrap_;
                public:
                    LocalF(ConnectionLocator const &locator)
                        : M::IExternalComponent()
                        , M::template AbstractOnOrderFacility<Req, Resp>()
                        , infra::RealTimeAppComponents<Env>::template ThreadedHandler<
                            typename M::template Key<Req>
                            , LocalF 
                        >()
                        , locator_(locator)
                        , httpMethod_(locator.query("http_method", ""))
                        , useGet_(
                            (locator.query("use_get", "false") == "true")
                            && 
                            (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>)
                        )
                        , simplePost_(
                            (locator.query("simple_post", "false") == "true")
                            && 
                            (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>)
                        )
                        , otherwiseEncodeInUrl_(
                            (locator.query("no_query_body", "false") == "true")
                            && 
                            (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>)
                        )
                        , noRequestResponseWrap_(
                            locator.query("no_wrap", "false") == "true"
                        )
                    {
                        this->startThread();
                    }
                    virtual void start(Env *env) override final {
                    }
                    void actuallyHandle(typename M::template InnerData<typename M::template Key<Req>> &&req) {
                        auto id = req.timedData.value.id();

                        nlohmann::json sendData;
                        std::ostringstream oss;
                        if constexpr (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>) {
                            if (useGet_ || simplePost_ || otherwiseEncodeInUrl_) {
                                if constexpr (!std::is_empty_v<Req>) {
                                    bool start = true;
                                    basic::struct_field_info_utils::StructFieldInfoBasedSimpleCsvOutput<Req>
                                        ::outputNameValuePairs(
                                            req.timedData.value.key()
                                            , [&start,&oss](std::string const &name, std::string const &value) {
                                                if (!start) {
                                                    oss << '&';
                                                }
                                                JsonRESTClientFacilityFactoryUtils::urlEscape(oss, name);
                                                oss << '=';
                                                JsonRESTClientFacilityFactoryUtils::urlEscape(oss, value);
                                                start = false;
                                            }
                                        );
                                }
                            } else {
                                basic::nlohmann_json_interop::JsonEncoder<Req>::write(sendData, (noRequestResponseWrap_?std::nullopt:std::optional<std::string> {"request"}), req.timedData.value.key());
                            }
                        } else {
                            basic::nlohmann_json_interop::JsonEncoder<Req>::write(sendData, (noRequestResponseWrap_?std::nullopt:std::optional<std::string> {"request"}), req.timedData.value.key());
                        }

                        std::optional<std::string> methodParam = std::nullopt;
                        if (httpMethod_ != "") {
                            methodParam = httpMethod_;
                        } else if (useGet_) {
                            methodParam = "GET";
                        }

                        auto *env = req.environment;
                        env->JsonRESTComponent::addJsonRESTClient(
                            locator_
                            , ((useGet_||otherwiseEncodeInUrl_)?oss.str():"")
                            , ((useGet_||otherwiseEncodeInUrl_)?"":(simplePost_?oss.str():sendData.dump()))
                            , [this,env,id=std::move(id)](unsigned status, std::string &&response) mutable {
                                if constexpr (std::is_same_v<Resp, RawString> || std::is_same_v<Resp, basic::ByteData>) {
                                    this->publish(
                                        env 
                                        , typename M::template Key<Resp> {
                                            std::move(id)
                                            , {std::move(response)}
                                        }
                                        , true
                                    );
                                } else if constexpr (std::is_same_v<Resp, RawStringWithStatus>) {
                                    this->publish(
                                        env 
                                        , typename M::template Key<Resp> {
                                            std::move(id)
                                            , {status, std::move(response)}
                                        }
                                        , true
                                    );
                                } else {
                                    try {
                                        nlohmann::json x = nlohmann::json::parse(response);
                                        Resp resp;
                                        basic::nlohmann_json_interop::Json<Resp *> r(&resp);
                                        if (r.fromNlohmannJson(noRequestResponseWrap_?x:x["response"])) {
                                            this->publish(
                                                env 
                                                , typename M::template Key<Resp> {
                                                    std::move(id)
                                                    , std::move(resp)
                                                }
                                                , true
                                            );
                                        }
                                    } catch (...) {
                                        env->log(infra::LogLevel::Error, "Cannot parse reply string '"+response+"' as json");
                                    }
                                }
                            }
                            , (simplePost_?"application/x-www-form-urlencoded":"application/json")
                            , methodParam
                        );
                    }
                    void idleWork() {}
                    void waitForStart() {}
                };
                return M::template fromAbstractOnOrderFacility<Req,Resp>(new LocalF(locator));
            } else {
                throw JsonRESTComponentException("json rest facility only works when the data types are json-encodable");
            }
        }

        template <class Req, class Resp>
        static std::future<Resp> typedOneShotRemoteCall(Env *env, ConnectionLocator const &rpcQueueLocator, Req &&request) {
            if constexpr (
                basic::nlohmann_json_interop::JsonWrappable<Req>::value
                &&
                basic::nlohmann_json_interop::JsonWrappable<Resp>::value
            ) {
                if constexpr(DetermineClientSideIdentityForRequest<Env, Req>::HasIdentity) {
                    if constexpr (!std::is_same_v<typename DetermineClientSideIdentityForRequest<Env, Req>::IdentityType, std::string>) {
                        throw JsonRESTComponentException("json rest typed one shot remote call does not work when the request has non-string identity");
                    }
                }
                std::string httpMethod = rpcQueueLocator.query("http_method", "");
                bool useGet = (
                    (rpcQueueLocator.query("use_get", "false") == "true")
                    && 
                    (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>)
                );
                bool simplePost = (
                    (rpcQueueLocator.query("simple_post", "false") == "true")
                    && 
                    (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>)
                );
                bool otherwiseEncodeInUrl = (
                    (rpcQueueLocator.query("no_query_body", "false") == "true")
                    && 
                    (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>)
                );
                bool noRequestResponseWrap = (
                    rpcQueueLocator.query("no_wrap", "false") == "true"
                );
                
                auto ret = std::make_shared<std::promise<Resp>>();
                nlohmann::json sendData;
                std::ostringstream oss;
                if constexpr (basic::struct_field_info_utils::IsStructFieldInfoBasedCsvCompatibleStruct<Req> || std::is_empty_v<Req>) {
                    if (useGet || simplePost || otherwiseEncodeInUrl) {
                        if constexpr (!std::is_empty_v<Req>) {
                            bool start = true;
                            basic::struct_field_info_utils::StructFieldInfoBasedSimpleCsvOutput<Req>
                                ::outputNameValuePairs(
                                    request
                                    , [&start,&oss](std::string const &name, std::string const &value) {
                                        if (!start) {
                                            oss << '&';
                                        }
                                        JsonRESTClientFacilityFactoryUtils::urlEscape(oss, name);
                                        oss << '=';
                                        JsonRESTClientFacilityFactoryUtils::urlEscape(oss, value);
                                        start = false;
                                    }
                                );
                        }
                    } else {
                        basic::nlohmann_json_interop::JsonEncoder<Req>::write(sendData, (noRequestResponseWrap?std::nullopt:std::optional<std::string> {"request"}), request);
                    }
                } else {
                    basic::nlohmann_json_interop::JsonEncoder<Req>::write(sendData, (noRequestResponseWrap?std::nullopt:std::optional<std::string> {"request"}), request);
                }
                std::optional<std::string> methodParam = std::nullopt;
                if (httpMethod != "") {
                    methodParam = httpMethod;
                } else if (useGet) {
                    methodParam = "GET";
                }
                env->JsonRESTComponent::addJsonRESTClient(
                    rpcQueueLocator
                    , ((useGet||otherwiseEncodeInUrl)?oss.str():"")
                    , ((useGet||otherwiseEncodeInUrl)?"":(simplePost?oss.str():sendData.dump()))
                    , [ret,env,noRequestResponseWrap](unsigned status, std::string &&response) mutable {
                        if constexpr (std::is_same_v<Resp, RawString> || std::is_same_v<Resp, basic::ByteData>) {
                            ret->set_value({std::move(response)});
                        } else if constexpr (std::is_same_v<Resp, RawStringWithStatus>) {
                            ret->set_value({status, std::move(response)});
                        } else {
                            try {
                                nlohmann::json x = nlohmann::json::parse(response);
                                Resp resp;
                                basic::nlohmann_json_interop::Json<Resp *> r(&resp);
                                if (r.fromNlohmannJson(noRequestResponseWrap?x:x["response"])) {
                                    ret->set_value(std::move(resp));
                                }
                            } catch (...) {
                                env->log(infra::LogLevel::Error, "Cannot parse reply string '"+response+"' as json");
                            }
                        }
                    }
                    , (simplePost?"x-www-form-urlencoded":"application/json")
                    , methodParam
                );
                return ret->get_future();
            } else {
                throw JsonRESTComponentException("json rest typed one shot remote call only works when the data types are json-encodable");
            }
        }
    };

}}}}}

#endif