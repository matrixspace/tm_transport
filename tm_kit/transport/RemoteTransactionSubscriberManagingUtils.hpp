#ifndef TM_KIT_TRANSPORT_REMOTE_TRANSACTION_SUBSCRIBER_MANAGING_UTILS_HPP_
#define TM_KIT_TRANSPORT_REMOTE_TRANSACTION_SUBSCRIBER_MANAGING_UTILS_HPP_

#include <tm_kit/transport/MultiTransportRemoteFacilityManagingUtils.hpp>

namespace dev { namespace cd606 { namespace tm { namespace transport {
    template <class R, class GS>
    class RemoteTransactionSubscriberManagingUtils {
    public:
        static auto createSubscriber(
            R &r 
            , typename R::template ConvertibleToSourceoid<HeartbeatMessage> &&heartbeatSource
            , std::regex const &serverNameRE
            , std::string const &facilityRegistrationName
            , typename GS::Subscription &&initialSubscriptionCommand
            , std::optional<typename R::template Source<basic::VoidStruct>> exitSource = std::nullopt
            , std::optional<ByteDataHookPair> hooks = std::nullopt
            , std::chrono::system_clock::duration ttl = std::chrono::seconds(3)
            , std::chrono::system_clock::duration checkPeriod = std::chrono::seconds(5)
        ) -> typename R::template Sourceoid<
            typename R::AppType::template KeyedData<
                typename GS::Input, typename GS::Output
            >
        > {
            using M = typename R::AppType;
            auto res = MultiTransportRemoteFacilityManagingUtils<R>::template setupOneDistinguishedRemoteFacility
            <typename GS::Input, typename GS::Output>(
                r 
                , std::move(heartbeatSource)
                , serverNameRE
                , facilityRegistrationName
                , [cmd=std::move(initialSubscriptionCommand)]() {
                    return typename GS::Input {
                        std::move(cmd)
                    };
                }
                , [](typename GS::Input const &, typename GS::Output const &o) {
                    return std::visit([](auto const &x) -> bool {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, typename GS::Subscription>) {
                            return true;
                        } else {
                            return false;
                        }
                    }, o.value);
                }
                , hooks 
                , ttl
                , checkPeriod
            );
            if (exitSource) {
                auto idStorage = std::make_shared<std::unordered_map<ConnectionLocator, typename M::EnvironmentType::IDType>>();
                auto idStorageMutex = std::make_shared<std::mutex>();
                r.preservePointer(idStorage);
                r.preservePointer(idStorageMutex);
                auto saveIDAndRemoveConnector = M::template kleisli<
                    typename M::template KeyedData<std::tuple<ConnectionLocator, typename GS::Input>, typename GS::Output> 
                >(
                    [idStorage,idStorageMutex](typename M::template InnerData<typename M::template KeyedData<std::tuple<ConnectionLocator, typename GS::Input>, typename GS::Output>> &&data) 
                        -> typename M::template Data<typename M::template KeyedData<typename GS::Input, typename GS::Output>>
                    {
                        auto id = data.timedData.value.key.id();
                        auto conn = std::get<0>(data.timedData.value.key.key());
                        auto *env = data.environment;
                        std::visit([env,idStorage,idStorageMutex,&id,&conn](auto const &x) {
                            using T = std::decay_t<decltype(x)>;
                            if constexpr (std::is_same_v<T, typename GS::Subscription>) {
                                std::lock_guard<std::mutex> _(
                                    *idStorageMutex
                                );
                                (*idStorage)[conn] = id;
                            } else if constexpr (std::is_same_v<T, typename GS::Unsubscription>) {
                                std::lock_guard<std::mutex> _(
                                    *idStorageMutex
                                );
                                for (auto const &item : *idStorage) {
                                    if (item.second == x.originalSubscriptionID) {
                                        idStorage->erase(item.first);
                                        break;
                                    }
                                }
                                if (idStorage->empty()) {
                                    env->exit();
                                }
                            }
                        }, data.timedData.value.data.value);
                        return typename M::template InnerData<typename M::template KeyedData<typename GS::Input, typename GS::Output>> {
                            env 
                            , {
                                env->resolveTime(data.timedData.timePoint)
                                , {
                                    {id, std::get<1>(data.timedData.value.key.key())}
                                    , std::move(data.timedData.value.data)
                                }
                                , data.timedData.finalFlag
                            }
                        };
                    }
                );
                r.registerAction("gs:"+facilityRegistrationName+"/saveIDAndRemoveConnector", saveIDAndRemoveConnector);
                res.feedOrderResults(r, r.actionAsSink(saveIDAndRemoveConnector));
                auto removeID = M::template pureExporter<
                    std::tuple<ConnectionLocator, bool>
                >(
                    [idStorage,idStorageMutex](std::tuple<ConnectionLocator, bool> &&d) {
                        if (!std::get<1>(d)) {
                            std::lock_guard<std::mutex> _(
                                *idStorageMutex
                            );
                            idStorage->erase(std::get<0>(d));
                        }
                    }
                );
                r.registerExporter("gs:"+facilityRegistrationName+"/removeID", removeID);
                res.feedConnectionChanges(r, r.exporterAsSink(removeID));
                auto unsubscribe = M::template kleisliMulti<basic::VoidStruct>(
                    [idStorage,idStorageMutex](typename M::template InnerData<basic::VoidStruct> &&d) -> typename M::template Data<std::vector<typename M::template Key<std::tuple<ConnectionLocator, typename GS::Input>>>>
                    {
                        std::lock_guard<std::mutex> _(
                            *idStorageMutex
                        );
                        if (idStorage->empty()) {
                            d.environment->exit();
                        }
                        std::vector<typename M::template Key<std::tuple<ConnectionLocator, typename GS::Input>>> ret;
                        for (auto const &s : *idStorage) {
                            ret.push_back(typename M::template Key<std::tuple<ConnectionLocator, typename GS::Input>>(
                                std::tuple<ConnectionLocator, typename GS::Input> {
                                    s.first, typename GS::Input {typename GS::Unsubscription {s.second}}
                                }
                            ));
                        }
                        return typename M::template InnerData<std::vector<typename M::template Key<std::tuple<ConnectionLocator, typename GS::Input>>>> {
                            d.environment 
                            , {
                                d.environment->resolveTime(d.timedData.timePoint)
                                , std::move(ret)
                                , true
                            }
                        };
                    }
                );
                r.registerAction("gs:"+facilityRegistrationName+"/unsubscribe", unsubscribe);
                res.orderReceiver(r, r.execute(unsubscribe, exitSource->clone()));
                return r.sourceAsSourceoid(r.actionAsSource(saveIDAndRemoveConnector));
            } else {
                auto removeConnector = M::template liftPure<
                    typename M::template KeyedData<std::tuple<ConnectionLocator, typename GS::Input>, typename GS::Output> 
                >(
                    [](typename M::template KeyedData<std::tuple<ConnectionLocator, typename GS::Input>, typename GS::Output> &&data) 
                        -> typename M::template KeyedData<typename GS::Input, typename GS::Output>
                    {
                        return {
                            {data.key.id(), std::get<1>(data.key.key())}
                            , std::move(data.data)
                        };
                    }
                );
                r.registerAction("gs:"+facilityRegistrationName+"/removeConnector", removeConnector);
                res.feedOrderResults(r, r.actionAsSink(removeConnector));
                return r.sourceAsSourceoid(r.actionAsSource(removeConnector));
            }
        }
    };
} } } }

#endif
